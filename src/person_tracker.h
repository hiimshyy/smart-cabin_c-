#pragma once

#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "tracker_types.h"
#include "yolo_post.h"

class PersonTracker {
public:
    virtual ~PersonTracker() = default;
    virtual void predict(const FrameContext& frame) = 0;
    virtual void update(const std::vector<PersonDet>& detections, const FrameContext& frame) = 0;
    virtual std::vector<TrackView> active_tracks() const = 0;
    virtual std::optional<TrackView> find_track(int track_id) const = 0;
    virtual bool needs_recognition(int track_id, double now_ms) const = 0;
    virtual void record_recognition(int track_id, int64_t resident_id,
                                    const std::string& display_name, float similarity,
                                    double now_ms) = 0;
    virtual std::vector<int> invalidate_residents(const std::set<int64_t>& changed_or_removed,
                                                  bool invalidate_unknown) = 0;
    virtual int active_count() const = 0;
    virtual int tracked_count() const = 0;
};

std::unique_ptr<PersonTracker> make_person_tracker(TrackerMode mode,
                                                   const TrackerConfig& config,
                                                   int legacy_max_missed,
                                                   int legacy_recognition_retry_frames);
