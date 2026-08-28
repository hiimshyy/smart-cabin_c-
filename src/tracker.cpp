#include "tracker.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

float bbox_iou(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2) {
    float xx1 = std::max(ax1, bx1);
    float yy1 = std::max(ay1, by1);
    float xx2 = std::min(ax2, bx2);
    float yy2 = std::min(ay2, by2);
    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float aa = std::max(0.0f, ax2 - ax1) * std::max(0.0f, ay2 - ay1);
    float ab = std::max(0.0f, bx2 - bx1) * std::max(0.0f, by2 - by1);
    float uni = aa + ab - inter;
    return (uni > 0) ? inter / uni : 0.0f;
}

void Tracker::update(const std::vector<PersonDet>& dets, int frame_idx) {
    const size_t N_det = dets.size();
    const size_t N_trk = tracks_.size();

    std::vector<bool> det_used(N_det, false);
    std::vector<bool> trk_used(N_trk, false);

    // ---- Step 1: build pairwise IoU list and greedy-match ----
    struct Pair { size_t d, t; float iou; };
    std::vector<Pair> pairs;
    pairs.reserve(N_det * N_trk);
    for (size_t d = 0; d < N_det; ++d) {
        const auto& dd = dets[d];
        for (size_t t = 0; t < N_trk; ++t) {
            if (!tracks_[t].alive) continue;
            const auto& tt = tracks_[t];
            float v = bbox_iou(dd.x1, dd.y1, dd.x2, dd.y2,
                               tt.x1, tt.y1, tt.x2, tt.y2);
            if (v >= iou_thresh) pairs.push_back({d, t, v});
        }
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const Pair& a, const Pair& b){ return a.iou > b.iou; });

    for (const auto& p : pairs) {
        if (det_used[p.d] || trk_used[p.t]) continue;
        det_used[p.d] = true;
        trk_used[p.t] = true;
        Track& t = tracks_[p.t];
        const PersonDet& d = dets[p.d];
        t.x1 = d.x1; t.y1 = d.y1; t.x2 = d.x2; t.y2 = d.y2;
        t.conf = d.score;
        t.missed_frames = 0;
        t.last_seen_frame = frame_idx;
        t.consecutive_hits++;
        if (t.state == TrackState::NEW && t.consecutive_hits >= confirm_hits) {
            t.state = TrackState::CONFIRMED;
        } else if (t.state == TrackState::LOST) {
            t.state = TrackState::CONFIRMED;
        }
    }

    // ---- Step 2: unmatched tracks -> miss handling ----
    for (size_t t = 0; t < N_trk; ++t) {
        if (trk_used[t]) continue;
        Track& tr = tracks_[t];
        if (!tr.alive) continue;
        tr.missed_frames++;
        tr.consecutive_hits = 0;

        if (tr.state == TrackState::NEW) {
            // Single-frame false positive — kill immediately.
            kill_track(t, frame_idx);
        } else if (tr.state == TrackState::CONFIRMED) {
            if (tr.missed_frames >= 3) tr.state = TrackState::LOST;
        } else if (tr.state == TrackState::LOST) {
            if (tr.missed_frames >= max_missed) kill_track(t, frame_idx);
        }
    }

    // ---- Step 3: unmatched detections -> new tracks ----
    for (size_t d = 0; d < N_det; ++d) {
        if (det_used[d]) continue;
        Track nt;
        nt.id = next_id_++;
        const PersonDet& dd = dets[d];
        nt.x1 = dd.x1; nt.y1 = dd.y1; nt.x2 = dd.x2; nt.y2 = dd.y2;
        nt.conf = dd.score;
        nt.state = TrackState::NEW;
        nt.consecutive_hits = 1;
        nt.first_seen_frame = frame_idx;
        nt.last_seen_frame  = frame_idx;
        tracks_.push_back(nt);
    }

    // ---- Step 4: compact killed tracks + expire ghosts ----
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
                       [](const Track& t){ return !t.alive; }),
        tracks_.end());
    expire_ghosts(frame_idx);
}

std::vector<Track*> Tracker::active_tracks() {
    std::vector<Track*> out;
    out.reserve(tracks_.size());
    for (auto& t : tracks_) {
        if (t.state == TrackState::CONFIRMED || t.state == TrackState::LOST)
            out.push_back(&t);
    }
    return out;
}

Track* Tracker::find_track_for_face(float cx, float cy) {
    Track* best = nullptr;
    float best_area = FLT_MAX;
    for (auto& t : tracks_) {
        if (t.state != TrackState::CONFIRMED) continue;
        if (cx < t.x1 || cx > t.x2) continue;
        if (cy < t.y1 || cy > t.y2) continue;
        // Face center should be in upper `head_ratio` fraction of person bbox
        float head_y_max = t.y1 + t.height() * head_ratio;
        if (cy > head_y_max) continue;
        float a = t.area();
        if (a > 0 && a < best_area) {
            best = &t;
            best_area = a;
        }
    }
    return best;
}

void Tracker::record_recognition(int track_id, const std::string& name,
                                 float sim, int frame_idx) {
    for (auto& t : tracks_) {
        if (t.id != track_id) continue;
        // If this is the FIRST successful recognition, try to inherit
        // identity from a ghost with the same name (elevator re-entry).
        bool was_unrecog = t.name.empty() || t.name == "unknown";
        t.name = name;
        t.match_sim = sim;
        t.last_recog_frame = frame_idx;
        if (was_unrecog && !name.empty() && name != "unknown") {
            try_inherit_identity(t, frame_idx);
        }
        return;
    }
}

bool Tracker::needs_recog(const Track& t, int frame_idx) const {
    if (t.state != TrackState::CONFIRMED) return false;
    if (t.name.empty()) return true;
    if (t.name == "unknown") {
        return (frame_idx - t.last_recog_frame) >= recog_unknown_retry;
    }
    return (frame_idx - t.last_recog_frame) >= recog_retry_frames;
}

int Tracker::active_count() const {
    int n = 0;
    for (const auto& t : tracks_)
        if (t.state == TrackState::CONFIRMED || t.state == TrackState::LOST) ++n;
    return n;
}

void Tracker::try_inherit_identity(Track& t, int frame_idx) {
    for (auto it = ghosts_.begin(); it != ghosts_.end(); ++it) {
        if (it->name == t.name) {
            // Inherit ID and first-seen time (accumulate session duration)
            int old_id = t.id;
            t.id = it->id;
            t.first_seen_frame = it->first_seen_frame;
            printf("[track] inherit identity '%s': ghost id=%d -> current id=%d (was %d)\n",
                   t.name.c_str(), it->id, t.id, old_id);
            ghosts_.erase(it);
            return;
        }
    }
}

void Tracker::kill_track(size_t idx, int frame_idx) {
    Track& t = tracks_[idx];
    t.alive = false;
    // Only push to ghost list if we had a known identity worth resurrecting.
    if (!t.name.empty() && t.name != "unknown") {
        Ghost g;
        g.id = t.id;
        g.name = t.name;
        g.first_seen_frame = t.first_seen_frame;
        g.killed_frame = frame_idx;
        g.x1 = t.x1; g.y1 = t.y1; g.x2 = t.x2; g.y2 = t.y2;
        ghosts_.push_back(g);
    }
}

void Tracker::expire_ghosts(int frame_idx) {
    ghosts_.erase(
        std::remove_if(ghosts_.begin(), ghosts_.end(),
            [&](const Ghost& g){
                return (frame_idx - g.killed_frame) > ghost_max_age_frames;
            }),
        ghosts_.end());
}
