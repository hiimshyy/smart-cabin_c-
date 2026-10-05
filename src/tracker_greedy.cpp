#include "tracker_greedy.h"

#include <algorithm>
#include <set>

GreedyIouTracker::GreedyIouTracker(const TrackerConfig& config, int max_missed,
                                   int recognition_retry_frames) {
    high_threshold_ = config.high_threshold;
    legacy_.iou_thresh = config.primary_iou_gate;
    legacy_.max_missed = max_missed;
    legacy_.confirm_hits = config.confirm_hits;
    legacy_.recog_retry_frames = recognition_retry_frames;
}

void GreedyIouTracker::predict(const FrameContext& frame) {
    // Legacy tracker has no motion model.  Keep the frame number only so its
    // frame-based recognition retry remains behaviorally compatible.
    last_frame_id_ = frame.frame_id;
}

void GreedyIouTracker::update(const std::vector<PersonDet>& detections, const FrameContext& frame) {
    last_frame_id_ = frame.frame_id;
    std::vector<PersonDet> high_detections;
    high_detections.reserve(detections.size());
    for (const PersonDet& det : detections) {
        if (det.score >= high_threshold_) high_detections.push_back(det);
    }
    legacy_.update(high_detections, frame.frame_id);
    std::set<int> live;
    for (Track* track : legacy_.active_tracks()) live.insert(track->id);
    for (auto it = resident_ids_.begin(); it != resident_ids_.end();) {
        if (!live.count(it->first)) it = resident_ids_.erase(it); else ++it;
    }
}

TrackView GreedyIouTracker::view_of(const Track& track) const {
    TrackView view;
    view.id = track.id;
    view.state = track.state == TrackState::LOST ? ByteTrackState::Lost : ByteTrackState::Tracked;
    view.box = {track.x1, track.y1, track.x2, track.y2};
    view.detection_score = track.conf;
    view.predicted_only = track.state == TrackState::LOST;
    view.identity.resident_id = -1;
    auto resident = resident_ids_.find(track.id);
    if (resident != resident_ids_.end()) view.identity.resident_id = resident->second;
    if (!track.name.empty() && track.name != "unknown") {
        view.identity.display_name = track.name;
        view.identity.similarity = track.match_sim;
        view.identity.latched = true;
    }
    return view;
}

std::vector<TrackView> GreedyIouTracker::active_tracks() const {
    std::vector<TrackView> views;
    // Legacy API is non-const because it returns mutable pointers.  The adapter
    // immediately copies data into immutable views and never exposes them.
    for (Track* track : const_cast<Tracker&>(legacy_).active_tracks()) views.push_back(view_of(*track));
    return views;
}

std::optional<TrackView> GreedyIouTracker::find_track(int track_id) const {
    for (const TrackView& view : active_tracks())
        if (view.id == track_id) return view;
    return std::nullopt;
}

bool GreedyIouTracker::needs_recognition(int track_id, double /*now_ms*/) const {
    for (Track* track : const_cast<Tracker&>(legacy_).active_tracks()) {
        if (track->id == track_id) return legacy_.needs_recog(*track, last_frame_id_);
    }
    return false;
}

void GreedyIouTracker::record_recognition(int track_id, int64_t resident_id,
                                          const std::string& display_name, float similarity,
                                          double /*now_ms*/) {
    legacy_.record_recognition(track_id, resident_id >= 0 ? display_name : "unknown", similarity,
                               last_frame_id_);
    if (resident_id >= 0) resident_ids_[track_id] = resident_id;
}

std::vector<int> GreedyIouTracker::invalidate_residents(const std::set<int64_t>& changed_or_removed,
                                                        bool invalidate_unknown) {
    std::set<int> selected;
    for (const TrackView& view : active_tracks()) {
        if (changed_or_removed.count(view.identity.resident_id) ||
            (invalidate_unknown && view.identity.resident_id < 0))
            selected.insert(view.id);
    }
    std::vector<int> invalidated = legacy_.invalidate_catalog_cache(selected, invalidate_unknown);
    for (int id : invalidated) resident_ids_.erase(id);
    return invalidated;
}

int GreedyIouTracker::active_count() const { return legacy_.active_count(); }

int GreedyIouTracker::tracked_count() const {
    int count = 0;
    for (const TrackView& view : active_tracks())
        if (view.state == ByteTrackState::Tracked) ++count;
    return count;
}
