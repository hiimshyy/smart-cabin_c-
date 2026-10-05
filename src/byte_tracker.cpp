#include "byte_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "linear_assignment.h"

namespace {

constexpr float kForbiddenCost = 1e6f;

bool finite(float value) { return std::isfinite(value); }

}  // namespace

ByteTracker::ByteTracker(TrackerConfig config) : config_(config) {}

bool ByteTracker::valid_detection(const PersonDet& det) {
    return finite(det.x1) && finite(det.y1) && finite(det.x2) && finite(det.y2) &&
           finite(det.score) && det.x2 > det.x1 && det.y2 > det.y1 &&
           det.score >= 0.0f;
}

BoxF ByteTracker::to_box(const PersonDet& det) {
    return {det.x1, det.y1, det.x2, det.y2};
}

float ByteTracker::iou(const BoxF& a, const BoxF& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float iw = std::max(0.0f, x2 - x1);
    const float ih = std::max(0.0f, y2 - y1);
    const float inter = iw * ih;
    const float aa = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    const float ab = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    const float uni = aa + ab - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

bool ByteTracker::detection_order(const PersonDet& a, const PersonDet& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.x1 != b.x1) return a.x1 < b.x1;
    if (a.y1 != b.y1) return a.y1 < b.y1;
    if (a.x2 != b.x2) return a.x2 < b.x2;
    return a.y2 < b.y2;
}

bool ByteTracker::is_confirmed(const InternalTrack& track) {
    return track.state == ByteTrackState::Tracked || track.state == ByteTrackState::Lost;
}

void ByteTracker::expire_at(double now_ms) {
    for (auto& track : tracks_) {
        if (!is_confirmed(track) || !std::isfinite(now_ms)) continue;
        if (now_ms - track.last_match_ms >= config_.lost_timeout_ms)
            track.state = ByteTrackState::Removed;
    }
}

void ByteTracker::predict(const FrameContext& frame) {
    expire_at(frame.timestamp_ms);
    for (auto& track : tracks_) {
        if (track.state == ByteTrackState::Removed) continue;
        track.motion.predict(frame.timestamp_ms, config_.max_predict_gap_ms);
        track.box = track.motion.box();
        if (is_confirmed(track)) track.predicted_only = true;
    }
    expire_at(frame.timestamp_ms);
}

std::vector<ByteTracker::Match> ByteTracker::associate(
    const std::vector<int>& track_indices, const std::vector<PersonDet>& detections,
    float iou_gate, std::vector<int>& unmatched_track_indices,
    std::vector<int>& unmatched_detection_indices) const {
    unmatched_track_indices = track_indices;
    unmatched_detection_indices.clear();
    for (int i = 0; i < static_cast<int>(detections.size()); ++i)
        unmatched_detection_indices.push_back(i);
    if (track_indices.empty() || detections.empty()) return {};

    std::vector<std::vector<float>> cost(track_indices.size(),
                                         std::vector<float>(detections.size(), kForbiddenCost));
    for (size_t row = 0; row < track_indices.size(); ++row) {
        const BoxF& predicted = tracks_[track_indices[row]].box;
        for (size_t col = 0; col < detections.size(); ++col) {
            const float overlap = iou(predicted, to_box(detections[col]));
            if (overlap >= iou_gate) cost[row][col] = 1.0f - overlap;
        }
    }
    Assignment assignment = solve_linear_assignment(cost, kForbiddenCost);
    std::vector<Match> matches;
    matches.reserve(assignment.matches.size());
    unmatched_track_indices.clear();
    unmatched_detection_indices.clear();
    for (const auto& pair : assignment.matches)
        matches.emplace_back(track_indices[pair.first], pair.second);
    for (int row : assignment.unmatched_rows)
        unmatched_track_indices.push_back(track_indices[row]);
    unmatched_detection_indices = assignment.unmatched_cols;
    return matches;
}

void ByteTracker::apply_match(InternalTrack& track, const PersonDet& detection,
                              const FrameContext& frame) {
    track.motion.correct(to_box(detection), frame.timestamp_ms);
    track.box = track.motion.box();
    track.score = detection.score;
    track.last_match_ms = frame.timestamp_ms;
    track.missed_updates = 0;
    track.predicted_only = false;
    ++track.hits;
    if (track.state == ByteTrackState::Tentative && track.hits >= config_.confirm_hits) {
        track.state = ByteTrackState::Tracked;
        track.identity = {};  // confirmed but unrecognized means Unknown, not an "untried" label
    } else if (track.state == ByteTrackState::Lost) {
        track.state = ByteTrackState::Tracked;
    }
}

void ByteTracker::create_tentative(const PersonDet& detection, const FrameContext& frame) {
    InternalTrack track;
    track.id = next_id_++;
    track.box = to_box(detection);
    track.motion.initialize(track.box, frame.timestamp_ms);
    track.score = detection.score;
    track.hits = 1;
    track.first_seen_ms = frame.timestamp_ms;
    track.last_match_ms = frame.timestamp_ms;
    tracks_.push_back(std::move(track));
}

void ByteTracker::resolve_duplicates() {
    // Only collapse a highly-overlapping pair that already claims the same
    // known resident.  Unknown overlapping people must remain separate.
    for (size_t a = 0; a < tracks_.size(); ++a) {
        if (!is_confirmed(tracks_[a]) || tracks_[a].identity.resident_id < 0) continue;
        for (size_t b = a + 1; b < tracks_.size(); ++b) {
            if (!is_confirmed(tracks_[b]) ||
                tracks_[a].identity.resident_id != tracks_[b].identity.resident_id ||
                iou(tracks_[a].box, tracks_[b].box) < 0.85f) continue;
            InternalTrack& keep = (tracks_[a].hits > tracks_[b].hits ||
                                   (tracks_[a].hits == tracks_[b].hits && tracks_[a].id < tracks_[b].id))
                                      ? tracks_[a] : tracks_[b];
            InternalTrack& remove = (&keep == &tracks_[a]) ? tracks_[b] : tracks_[a];
            remove.state = ByteTrackState::Removed;
        }
    }
}

void ByteTracker::update(const std::vector<PersonDet>& raw_detections,
                         const FrameContext& frame) {
    expire_at(frame.timestamp_ms);
    std::vector<PersonDet> high, low;
    for (const auto& det : raw_detections) {
        if (!valid_detection(det) || det.score < config_.low_threshold) continue;
        if (det.score >= config_.high_threshold) high.push_back(det);
        else low.push_back(det);
    }
    std::sort(high.begin(), high.end(), detection_order);
    std::sort(low.begin(), low.end(), detection_order);

    std::vector<int> primary_tracks;
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i)
        if (is_confirmed(tracks_[i])) primary_tracks.push_back(i);
    std::sort(primary_tracks.begin(), primary_tracks.end(), [&](int a, int b) {
        return tracks_[a].id < tracks_[b].id;
    });

    std::vector<int> primary_unmatched, high_remaining;
    for (const Match& match : associate(primary_tracks, high, config_.primary_iou_gate,
                                        primary_unmatched, high_remaining))
        apply_match(tracks_[match.first], high[match.second], frame);

    // Low confidence detections may rescue only tracks that were Tracked at
    // the beginning of this update.  Stale Lost tracks cannot be resurrected
    // by low-confidence noise.
    std::vector<int> secondary_tracks;
    for (int index : primary_unmatched)
        if (tracks_[index].state == ByteTrackState::Tracked) secondary_tracks.push_back(index);
    std::vector<int> secondary_unmatched, low_remaining;
    for (const Match& match : associate(secondary_tracks, low, config_.secondary_iou_gate,
                                        secondary_unmatched, low_remaining))
        apply_match(tracks_[match.first], low[match.second], frame);

    std::vector<int> tentative_tracks;
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i)
        if (tracks_[i].state == ByteTrackState::Tentative) tentative_tracks.push_back(i);
    std::sort(tentative_tracks.begin(), tentative_tracks.end(), [&](int a, int b) {
        return tracks_[a].id < tracks_[b].id;
    });
    std::vector<PersonDet> remaining_high;
    remaining_high.reserve(high_remaining.size());
    for (int index : high_remaining) remaining_high.push_back(high[index]);
    std::vector<int> tentative_unmatched, creation_high;
    for (const Match& match : associate(tentative_tracks, remaining_high, config_.primary_iou_gate,
                                        tentative_unmatched, creation_high))
        apply_match(tracks_[match.first], remaining_high[match.second], frame);
    for (int index : tentative_unmatched) tracks_[index].state = ByteTrackState::Removed;
    for (int index : creation_high) create_tentative(remaining_high[index], frame);

    // Miss handling runs only on detector updates, never on predictor-only
    // frames.  The absolute timeout is still enforced by predict()/expire_at.
    for (int index : secondary_unmatched) {
        InternalTrack& track = tracks_[index];
        if (track.state != ByteTrackState::Tracked) continue;
        ++track.missed_updates;
        track.predicted_only = true;
        const double since_match = frame.timestamp_ms - track.last_match_ms;
        if (track.missed_updates >= config_.lost_grace_min_updates &&
            since_match >= config_.lost_grace_ms)
            track.state = ByteTrackState::Lost;
    }
    expire_at(frame.timestamp_ms);
    resolve_duplicates();
}

TrackView ByteTracker::view_of(const InternalTrack& track) const {
    TrackView view;
    view.id = track.id;
    view.state = track.state;
    view.box = track.box;
    view.detection_score = track.score;
    view.predicted_only = track.predicted_only;
    view.identity = track.identity;
    return view;
}

std::vector<TrackView> ByteTracker::active_tracks() const {
    std::vector<TrackView> out;
    for (const auto& track : tracks_)
        if (is_confirmed(track)) out.push_back(view_of(track));
    return out;
}

std::optional<TrackView> ByteTracker::find_track(int track_id) const {
    for (const auto& track : tracks_)
        if (track.id == track_id && track.state != ByteTrackState::Removed) return view_of(track);
    return std::nullopt;
}

bool ByteTracker::needs_recognition(int track_id, double now_ms) const {
    for (const auto& track : tracks_) {
        if (track.id != track_id) continue;
        if (track.state != ByteTrackState::Tracked || track.predicted_only) return false;
        if (track.identity.latched && track.identity.resident_id >= 0) return false;
        return track.identity.last_recognition_attempt_ms < 0.0 ||
            now_ms - track.identity.last_recognition_attempt_ms >= config_.unknown_retry_ms;
    }
    return false;
}

void ByteTracker::record_recognition(int track_id, int64_t resident_id,
                                     const std::string& display_name, float similarity,
                                     double now_ms) {
    for (auto& track : tracks_) {
        if (track.id != track_id || track.state == ByteTrackState::Removed) continue;
        if (track.identity.latched && track.identity.resident_id >= 0 && resident_id < 0) return;
        track.identity.last_recognition_attempt_ms = now_ms;
        track.identity.last_recognition_ms = now_ms;
        if (resident_id >= 0) {
            track.identity.resident_id = resident_id;
            track.identity.display_name = display_name;
            track.identity.similarity = similarity;
            track.identity.latched = true;
        } else if (!track.identity.latched) {
            track.identity.resident_id = -1;
            track.identity.display_name.clear();
            track.identity.similarity = similarity;
        }
        return;
    }
}

std::vector<int> ByteTracker::invalidate_residents(const std::set<int64_t>& changed_or_removed,
                                                    bool invalidate_unknown) {
    std::vector<int> invalidated;
    for (auto& track : tracks_) {
        if (!is_confirmed(track)) continue;
        const bool unknown = track.identity.resident_id < 0;
        if (!changed_or_removed.count(track.identity.resident_id) && !(invalidate_unknown && unknown))
            continue;
        track.identity = {};
        invalidated.push_back(track.id);
    }
    return invalidated;
}

int ByteTracker::active_count() const {
    int count = 0;
    for (const auto& track : tracks_) if (is_confirmed(track)) ++count;
    return count;
}

int ByteTracker::tracked_count() const {
    int count = 0;
    for (const auto& track : tracks_)
        if (track.state == ByteTrackState::Tracked) ++count;
    return count;
}
