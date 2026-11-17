#pragma once

#include "occupancy_config.h"
#include "occupancy_state.h"
#include "tracker_types.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

enum class OccupancyEventDirection { Entry, Exit };

struct OccupancyEvent {
    OccupancyEventDirection direction    = OccupancyEventDirection::Entry;
    int                     track_id     = -1;
    double                  timestamp_ms = 0.0;
    std::uint64_t           occupant_id  = 0;
};

struct OccupancySnapshot {
    int                                        count      = 0;
    OccupancyConfidence                        confidence = OccupancyConfidence::Uncertain;
    std::vector<OccupancyEvent>                events;
    std::vector<std::pair<int, std::uint64_t>> track_occupants;
    bool                                       pipeline_unreliable = false;
};

using OccupancyStateChangeCallback = std::function<void(int, OccupancyConfidence)>;
using OccupancyTrackTraceCallback  = std::function<void(int, int, int, bool, bool)>;

class OccupancyCounter {
public:
    OccupancyCounter(OccupancyConfig cfg, OccupancyState initial_state,
                     OccupancyStateChangeCallback state_change = {},
                     OccupancyTrackTraceCallback  track_trace  = {});

    OccupancySnapshot update(const std::vector<TrackView>& tracks, int frame_w, int frame_h,
                             double timestamp_ms, bool frame_valid, bool tracking_unreliable);

    void reconcile(int count);
    void reconcile_empty();

    int count() const {
        return count_;
    }
    OccupancyConfidence confidence() const {
        return confidence_;
    }
    const OccupancyConfig& config() const {
        return cfg_;
    }

private:
    struct ZoneGeometry {
        std::vector<PointF> polygon;
        PointF              centroid{0.0f, 0.0f};
        PointF              inside_dir{0.0f, 1.0f};
        float               min_proj               = 0.0f;
        float               max_proj               = 0.0f;
        float               displacement_threshold = 0.0f;
    };

    struct TrackState {
        int           track_id            = -1;
        int           side                = 0;
        bool          in_zone             = false;
        bool          entry_candidate     = false;
        bool          exit_candidate      = false;
        bool          crossing_suppressed = false;
        bool          occupancy_assigned  = false;
        std::uint64_t occupant_id         = 0;
        float         start_proj          = 0.0f;
        float         extremum_proj       = 0.0f;
        float         last_nx             = 0.0f;
        float         last_ny             = 0.0f;
        double        last_seen_ms        = -1.0;
    };

    struct Ghost {
        int           track_id           = -1;
        int           side               = 0;
        bool          in_zone            = false;
        bool          entry_candidate    = false;
        bool          exit_candidate     = false;
        bool          occupancy_assigned = false;
        std::uint64_t occupant_id        = 0;
        float         start_proj         = 0.0f;
        float         extremum_proj      = 0.0f;
        float         last_nx            = 0.0f;
        float         last_ny            = 0.0f;
        double        expired_at_ms      = -1.0;
    };

    OccupancyConfig                     cfg_;
    int                                 count_                = 0;
    int                                 unassigned_occupants_ = 0;
    std::uint64_t                       next_occupant_id_     = 1;
    OccupancyConfidence                 confidence_           = OccupancyConfidence::Uncertain;
    OccupancyStateChangeCallback        state_change_;
    OccupancyTrackTraceCallback         track_trace_;
    std::vector<ZoneGeometry>           zone_geom_;
    std::unordered_map<int, TrackState> tracks_;
    std::vector<Ghost>                  ghosts_;

    std::vector<std::pair<int, std::uint64_t>> collect_track_occupants() const;

    void set_uncertain(const char* reason);
    void notify_if_changed(int previous_count, OccupancyConfidence previous_confidence);
    void remove_expired_ghosts(double now_ms);
    bool try_transfer_ghost(int track_id, float nx, float ny, double now_ms);
    void handle_track_lost(const TrackState& state, double now_ms);
};
