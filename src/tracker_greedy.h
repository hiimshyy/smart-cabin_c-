#pragma once

#include <map>

#include "person_tracker.h"
#include "tracker.h"  // Legacy greedy engine, kept only as rollout fallback.

class GreedyIouTracker final : public PersonTracker {
public:
    GreedyIouTracker(const TrackerConfig& config, int max_missed, int recognition_retry_frames);

    void predict(const FrameContext& frame) override;
    void update(const std::vector<PersonDet>& detections, const FrameContext& frame) override;
    std::vector<TrackView> active_tracks() const override;
    std::optional<TrackView> find_track(int track_id) const override;
    bool needs_recognition(int track_id, double now_ms) const override;
    void record_recognition(int track_id, int64_t resident_id,
                            const std::string& display_name, float similarity,
                            double now_ms) override;
    std::vector<int> invalidate_residents(const std::set<int64_t>& changed_or_removed,
                                          bool invalidate_unknown) override;
    int active_count() const override;
    int tracked_count() const override;

private:
    TrackView view_of(const Track& track) const;

    Tracker legacy_;
    float high_threshold_ = 0.5f;
    int last_frame_id_ = 0;
    std::map<int, int64_t> resident_ids_;
};
