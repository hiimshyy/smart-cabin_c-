#pragma once

#include <optional>
#include <set>
#include <vector>

#include "kalman_box_tracker.h"
#include "tracker_types.h"
#include "yolo_post.h"

// Single-threaded ByteTrack-lite backend.  It owns all mutable Track state and
// exposes immutable snapshots only; no caller can retain a vector element.
class ByteTracker {
public:
    explicit ByteTracker(TrackerConfig config = {});

    void predict(const FrameContext& frame);
    void update(const std::vector<PersonDet>& detections, const FrameContext& frame);

    std::vector<TrackView> active_tracks() const;
    std::optional<TrackView> find_track(int track_id) const;
    bool needs_recognition(int track_id, double now_ms) const;
    void record_recognition(int track_id, int64_t resident_id,
                            const std::string& display_name, float similarity,
                            double now_ms);
    std::vector<int> invalidate_residents(const std::set<int64_t>& changed_or_removed,
                                          bool invalidate_unknown);

    int active_count() const;
    int tracked_count() const;
    const TrackerConfig& config() const { return config_; }

private:
    struct InternalTrack {
        int id = -1;
        ByteTrackState state = ByteTrackState::Tentative;
        KalmanBoxTracker motion;
        BoxF box;
        float score = 0.0f;
        int hits = 0;
        int missed_updates = 0;
        double first_seen_ms = 0.0;
        double last_match_ms = 0.0;
        bool predicted_only = false;
        TrackIdentity identity;
    };

    using Match = std::pair<int, int>;  // indices into supplied track/detection pools

    static bool valid_detection(const PersonDet& det);
    static BoxF to_box(const PersonDet& det);
    static float iou(const BoxF& a, const BoxF& b);
    static bool detection_order(const PersonDet& a, const PersonDet& b);

    std::vector<Match> associate(const std::vector<int>& track_indices,
                                 const std::vector<PersonDet>& detections,
                                 float iou_gate,
                                 std::vector<int>& unmatched_track_indices,
                                 std::vector<int>& unmatched_detection_indices) const;
    void apply_match(InternalTrack& track, const PersonDet& detection, const FrameContext& frame);
    void create_tentative(const PersonDet& detection, const FrameContext& frame);
    void expire_at(double now_ms);
    void resolve_duplicates();
    TrackView view_of(const InternalTrack& track) const;
    static bool is_confirmed(const InternalTrack& track);

    TrackerConfig config_;
    std::vector<InternalTrack> tracks_;
    int next_id_ = 1;
};
