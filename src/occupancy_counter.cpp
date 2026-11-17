#include "occupancy_counter.h"

#include "log/logger.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace {

constexpr double GHOST_TIMEOUT_MS              = 500.0;
constexpr double FIRST_OBSERVATION_MAX_AGE_MS  = 1000.0;
constexpr float  GHOST_DISTANCE_THRESHOLD_NORM = 0.05f;
constexpr float  SIDE_EPSILON_NORM             = 1e-5f;
constexpr float  PI                            = 3.14159265358979323846f;

PointF foot_point_norm(const BoxF& box, int frame_w, int frame_h) {
    return PointF{(box.x1 + box.x2) * 0.5f / static_cast<float>(frame_w),
                  box.y2 / static_cast<float>(frame_h)};
}

PointF polygon_centroid(const std::vector<PointF>& polygon) {
    PointF center{0.0f, 0.0f};
    for (const PointF& point : polygon) {
        center.x += point.x;
        center.y += point.y;
    }
    if (!polygon.empty()) {
        center.x /= static_cast<float>(polygon.size());
        center.y /= static_cast<float>(polygon.size());
    }
    return center;
}

float dot(const PointF& left, const PointF& right) {
    return left.x * right.x + left.y * right.y;
}

bool point_in_polygon(const PointF& point, const std::vector<PointF>& polygon) {
    bool         inside = false;
    const size_t count  = polygon.size();
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        const PointF& current    = polygon[i];
        const PointF& previous   = polygon[j];
        const bool    intersects = ((current.y > point.y) != (previous.y > point.y)) &&
                                (point.x < (previous.x - current.x) * (point.y - current.y) /
                                                   (previous.y - current.y + 1e-12f) +
                                               current.x);
        if (intersects) inside = !inside;
    }
    return inside;
}

float cross_product(const PointF& first, const PointF& second, const PointF& third) {
    return (second.x - first.x) * (third.y - first.y) - (second.y - first.y) * (third.x - first.x);
}

bool on_segment(const PointF& first, const PointF& second, const PointF& point) {
    constexpr float EPSILON = 1e-6f;
    return std::fabs(cross_product(first, second, point)) <= EPSILON &&
           point.x >= std::min(first.x, second.x) - EPSILON &&
           point.x <= std::max(first.x, second.x) + EPSILON &&
           point.y >= std::min(first.y, second.y) - EPSILON &&
           point.y <= std::max(first.y, second.y) + EPSILON;
}

bool segments_intersect(const PointF& first_start, const PointF& first_end,
                        const PointF& second_start, const PointF& second_end) {
    const float first_side  = cross_product(first_start, first_end, second_start);
    const float second_side = cross_product(first_start, first_end, second_end);
    const float third_side  = cross_product(second_start, second_end, first_start);
    const float fourth_side = cross_product(second_start, second_end, first_end);
    if (((first_side > 0.0f && second_side < 0.0f) || (first_side < 0.0f && second_side > 0.0f)) &&
        ((third_side > 0.0f && fourth_side < 0.0f) || (third_side < 0.0f && fourth_side > 0.0f))) {
        return true;
    }
    return on_segment(first_start, first_end, second_start) ||
           on_segment(first_start, first_end, second_end) ||
           on_segment(second_start, second_end, first_start) ||
           on_segment(second_start, second_end, first_end);
}

bool movement_crosses_polygon(const PointF& start, const PointF& end,
                              const std::vector<PointF>& polygon) {
    for (size_t index = 0; index < polygon.size(); ++index) {
        const PointF& edge_start = polygon[index];
        const PointF& edge_end   = polygon[(index + 1) % polygon.size()];
        if (segments_intersect(start, end, edge_start, edge_end)) return true;
    }
    return false;
}

} // namespace

OccupancyCounter::OccupancyCounter(OccupancyConfig config, OccupancyState initial_state,
                                   OccupancyStateChangeCallback state_change,
                                   OccupancyTrackTraceCallback  track_trace)
    : cfg_(std::move(config)), count_(std::max(0, initial_state.count)),
      unassigned_occupants_(count_), confidence_(initial_state.confidence),
      state_change_(std::move(state_change)), track_trace_(std::move(track_trace)) {
    for (const CountingZone& zone : cfg_.zones) {
        ZoneGeometry geometry;
        geometry.polygon    = zone.polygon;
        geometry.centroid   = polygon_centroid(geometry.polygon);
        const float radians = zone.inside_direction_degrees * PI / 180.0f;
        geometry.inside_dir = PointF{std::cos(radians), std::sin(radians)};
        bool first          = true;
        for (const PointF& vertex : geometry.polygon) {
            const PointF relative{vertex.x - geometry.centroid.x, vertex.y - geometry.centroid.y};
            const float  projection = dot(relative, geometry.inside_dir);
            if (first || projection < geometry.min_proj) geometry.min_proj = projection;
            if (first || projection > geometry.max_proj) geometry.max_proj = projection;
            first = false;
        }
        geometry.displacement_threshold = zone.min_crossing_displacement;
        zone_geom_.push_back(std::move(geometry));
    }
}

std::vector<std::pair<int, std::uint64_t>> OccupancyCounter::collect_track_occupants() const {
    std::vector<std::pair<int, std::uint64_t>> out;
    for (const auto& kv : tracks_) {
        if (kv.second.occupancy_assigned) {
            out.emplace_back(kv.first, kv.second.occupant_id);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void OccupancyCounter::set_uncertain(const char* /*reason*/) {
    confidence_ = OccupancyConfidence::Uncertain;
}

void OccupancyCounter::notify_if_changed(int                 previous_count,
                                         OccupancyConfidence previous_confidence) {
    if (state_change_ && (previous_count != count_ || previous_confidence != confidence_)) {
        state_change_(count_, confidence_);
    }
}

void OccupancyCounter::remove_expired_ghosts(double now_ms) {
    for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        if (now_ms <= it->expired_at_ms) {
            ++it;
            continue;
        }
        if (it->occupancy_assigned) ++unassigned_occupants_;
        it = ghosts_.erase(it);
    }
}

bool OccupancyCounter::try_transfer_ghost(int track_id, float nx, float ny, double now_ms) {
    const float max_distance_squared =
        GHOST_DISTANCE_THRESHOLD_NORM * GHOST_DISTANCE_THRESHOLD_NORM;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < ghosts_.size(); ++i) {
        const float dx = ghosts_[i].last_nx - nx;
        const float dy = ghosts_[i].last_ny - ny;
        if (dx * dx + dy * dy <= max_distance_squared) candidates.push_back(i);
    }
    if (candidates.size() > 1) {
        set_uncertain("ambiguous track ID churn");
        return false;
    }
    if (candidates.empty()) return false;

    const size_t index = candidates.front();
    const Ghost  ghost = ghosts_[index];
    TrackState   state;
    state.track_id           = track_id;
    state.side               = ghost.side;
    state.in_zone            = ghost.in_zone;
    state.entry_candidate    = ghost.entry_candidate;
    state.exit_candidate     = ghost.exit_candidate;
    state.occupancy_assigned = ghost.occupancy_assigned;
    state.occupant_id        = ghost.occupant_id;
    state.start_proj         = ghost.start_proj;
    state.extremum_proj      = ghost.extremum_proj;
    state.last_nx            = nx;
    state.last_ny            = ny;
    state.last_seen_ms       = now_ms;
    tracks_.emplace(track_id, state);
    ghosts_.erase(ghosts_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void OccupancyCounter::handle_track_lost(const TrackState& state, double now_ms) {
    if (state.in_zone || state.entry_candidate || state.exit_candidate) {
        set_uncertain("track lost in counting zone");
    } else if (state.side == 1) {
        set_uncertain("track lost while inside cabin");
    }
    if (state.crossing_suppressed) return;

    Ghost ghost;
    ghost.track_id           = state.track_id;
    ghost.side               = state.side;
    ghost.in_zone            = state.in_zone;
    ghost.entry_candidate    = state.entry_candidate;
    ghost.exit_candidate     = state.exit_candidate;
    ghost.occupancy_assigned = state.occupancy_assigned;
    ghost.occupant_id        = state.occupant_id;
    ghost.start_proj         = state.start_proj;
    ghost.extremum_proj      = state.extremum_proj;
    ghost.last_nx            = state.last_nx;
    ghost.last_ny            = state.last_ny;
    ghost.expired_at_ms      = now_ms + GHOST_TIMEOUT_MS;
    ghosts_.push_back(ghost);
}

OccupancySnapshot OccupancyCounter::update(const std::vector<TrackView>& tracks, int frame_w,
                                           int frame_h, double timestamp_ms, bool frame_valid,
                                           bool tracking_unreliable) {
    const int                 previous_count      = count_;
    const OccupancyConfidence previous_confidence = confidence_;
    OccupancySnapshot         snapshot;

    if (!frame_valid || frame_w <= 0 || frame_h <= 0) {
        set_uncertain("frame invalid / RTSP outage");
        snapshot.count               = count_;
        snapshot.confidence          = confidence_;
        snapshot.track_occupants     = collect_track_occupants();
        snapshot.pipeline_unreliable = true;
        notify_if_changed(previous_count, previous_confidence);
        return snapshot;
    }
    if (zone_geom_.size() != 1) {
        set_uncertain("invalid counting-zone geometry");
        snapshot.count               = count_;
        snapshot.confidence          = confidence_;
        snapshot.track_occupants     = collect_track_occupants();
        snapshot.pipeline_unreliable = true;
        notify_if_changed(previous_count, previous_confidence);
        return snapshot;
    }

    remove_expired_ghosts(timestamp_ms);
    struct ReliableObservation {
        int    id;
        PointF foot;
    };
    std::unordered_map<int, bool>    current_ids;
    std::vector<ReliableObservation> reliable_observations;
    bool                             prediction_only_seen = false;
    for (const TrackView& track : tracks) {
        if (track.predicted_only) {
            prediction_only_seen = true;
            set_uncertain("tracking is prediction-only");
        }
        if (track.state == ByteTrackState::Tracked && !track.predicted_only) {
            current_ids[track.id] = true;
            reliable_observations.push_back(
                {track.id, foot_point_norm(track.box, frame_w, frame_h)});
        }
    }

    // Restore a track that temporarily went Lost/predicted_only under the SAME
    // track.id before running spatial-only ghost/ambiguity matching.
    for (const ReliableObservation& observation : reliable_observations) {
        if (tracks_.find(observation.id) != tracks_.end()) continue;
        auto git = std::find_if(ghosts_.begin(), ghosts_.end(), [&](const Ghost& ghost) {
            return ghost.track_id == observation.id;
        });
        if (git == ghosts_.end()) continue;
        const Ghost ghost = *git;
        TrackState  state;
        state.track_id           = ghost.track_id;
        state.side               = ghost.side;
        state.in_zone            = ghost.in_zone;
        state.entry_candidate    = ghost.entry_candidate;
        state.exit_candidate     = ghost.exit_candidate;
        state.occupancy_assigned = ghost.occupancy_assigned;
        state.occupant_id        = ghost.occupant_id;
        state.start_proj         = ghost.start_proj;
        state.extremum_proj      = ghost.extremum_proj;
        state.last_nx            = ghost.last_nx;
        state.last_ny            = ghost.last_ny;
        state.last_seen_ms       = ghost.expired_at_ms - GHOST_TIMEOUT_MS;
        tracks_.emplace(state.track_id, state);
        ghosts_.erase(git);
    }

    std::vector<int> missing_ids;
    for (const auto& tracked : tracks_) {
        if (current_ids.find(tracked.first) == current_ids.end()) {
            missing_ids.push_back(tracked.first);
        }
    }

    const float max_distance_squared =
        GHOST_DISTANCE_THRESHOLD_NORM * GHOST_DISTANCE_THRESHOLD_NORM;
    std::vector<std::pair<int, ReliableObservation>> direct_transfers;
    std::unordered_set<int>                          ambiguous_predecessor_ids;
    std::unordered_set<int>                          ambiguous_successor_ids;
    for (int missing_id : missing_ids) {
        const TrackState&                missing = tracks_.at(missing_id);
        std::vector<ReliableObservation> successors;
        for (const ReliableObservation& observation : reliable_observations) {
            if (tracks_.find(observation.id) != tracks_.end()) continue;
            const float dx = missing.last_nx - observation.foot.x;
            const float dy = missing.last_ny - observation.foot.y;
            if (dx * dx + dy * dy <= max_distance_squared) successors.push_back(observation);
        }
        if (successors.size() > 1) {
            set_uncertain("one-to-many track ID churn");
            ambiguous_predecessor_ids.insert(missing_id);
            for (const ReliableObservation& successor : successors) {
                ambiguous_successor_ids.insert(successor.id);
            }
            continue;
        }
        if (successors.size() != 1) continue;

        const ReliableObservation& successor    = successors.front();
        size_t                     predecessors = 0;
        for (int other_id : missing_ids) {
            const TrackState& other = tracks_.at(other_id);
            const float       dx    = other.last_nx - successor.foot.x;
            const float       dy    = other.last_ny - successor.foot.y;
            if (dx * dx + dy * dy <= max_distance_squared) ++predecessors;
        }
        for (const Ghost& ghost : ghosts_) {
            const float dx = ghost.last_nx - successor.foot.x;
            const float dy = ghost.last_ny - successor.foot.y;
            if (dx * dx + dy * dy <= max_distance_squared) ++predecessors;
        }
        if (predecessors == 1) direct_transfers.emplace_back(missing_id, successor);
    }

    for (const auto& transfer : direct_transfers) {
        auto old = tracks_.find(transfer.first);
        if (old == tracks_.end() || tracks_.find(transfer.second.id) != tracks_.end() ||
            ambiguous_predecessor_ids.count(transfer.first) != 0 ||
            ambiguous_successor_ids.count(transfer.second.id) != 0) {
            continue;
        }
        TrackState state   = old->second;
        state.track_id     = transfer.second.id;
        state.last_nx      = transfer.second.foot.x;
        state.last_ny      = transfer.second.foot.y;
        state.last_seen_ms = timestamp_ms;
        tracks_.erase(old);
        tracks_.emplace(state.track_id, state);
    }

    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (current_ids.find(it->first) == current_ids.end()) {
            if (ambiguous_predecessor_ids.count(it->first) == 0) {
                handle_track_lost(it->second, timestamp_ms);
            } else if (it->second.occupancy_assigned) {
                ++unassigned_occupants_;
            }
            it = tracks_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        std::vector<int> successors;
        for (const ReliableObservation& observation : reliable_observations) {
            if (tracks_.find(observation.id) != tracks_.end()) continue;
            const float dx = it->last_nx - observation.foot.x;
            const float dy = it->last_ny - observation.foot.y;
            if (dx * dx + dy * dy <= max_distance_squared) {
                successors.push_back(observation.id);
            }
        }
        if (successors.size() > 1) {
            set_uncertain("one-to-many ghost handoff");
            ambiguous_successor_ids.insert(successors.begin(), successors.end());
            if (it->occupancy_assigned) ++unassigned_occupants_;
            it = ghosts_.erase(it);
        } else {
            ++it;
        }
    }

    const ZoneGeometry& geometry = zone_geom_.front();
    for (const TrackView& track : tracks) {
        if (track.state != ByteTrackState::Tracked || track.predicted_only) continue;

        const PointF foot    = foot_point_norm(track.box, frame_w, frame_h);
        const bool   in_zone = point_in_polygon(foot, geometry.polygon);
        const PointF relative{foot.x - geometry.centroid.x, foot.y - geometry.centroid.y};
        const float  projection = dot(relative, geometry.inside_dir);
        int          new_side   = 0;
        if (!in_zone && projection < geometry.min_proj - SIDE_EPSILON_NORM) {
            new_side = -1;
        } else if (!in_zone && projection > geometry.max_proj + SIDE_EPSILON_NORM) {
            new_side = 1;
        }

        auto it                                 = tracks_.find(track.id);
        bool initialized_from_first_observation = false;
        if (it == tracks_.end()) {
            const bool ambiguous_successor = ambiguous_successor_ids.count(track.id) != 0;
            if (ambiguous_successor ||
                !try_transfer_ghost(track.id, foot.x, foot.y, timestamp_ms)) {
                const bool use_first_observation =
                    track.has_first_observation && track.first_observation_ms >= 0.0 &&
                    timestamp_ms >= track.first_observation_ms &&
                    timestamp_ms - track.first_observation_ms <= FIRST_OBSERVATION_MAX_AGE_MS;
                initialized_from_first_observation = use_first_observation;
                PointF initial_foot                = foot;
                bool   initial_in_zone             = in_zone;
                int    initial_side                = new_side;
                if (use_first_observation) {
                    initial_foot = foot_point_norm(track.first_observation_box, frame_w, frame_h);
                    initial_in_zone = point_in_polygon(initial_foot, geometry.polygon);
                    const PointF initial_relative{initial_foot.x - geometry.centroid.x,
                                                  initial_foot.y - geometry.centroid.y};
                    const float  initial_projection = dot(initial_relative, geometry.inside_dir);
                    initial_side                    = 0;
                    if (!initial_in_zone &&
                        initial_projection < geometry.min_proj - SIDE_EPSILON_NORM) {
                        initial_side = -1;
                    } else if (!initial_in_zone &&
                               initial_projection > geometry.max_proj + SIDE_EPSILON_NORM) {
                        initial_side = 1;
                    }
                }

                TrackState state;
                state.track_id            = track.id;
                state.side                = initial_side;
                state.in_zone             = initial_in_zone;
                state.crossing_suppressed = ambiguous_successor;
                state.last_nx             = initial_foot.x;
                state.last_ny             = initial_foot.y;
                state.last_seen_ms =
                    use_first_observation ? track.first_observation_ms : timestamp_ms;
                if (!ambiguous_successor && state.side == 1 && unassigned_occupants_ > 0) {
                    state.occupancy_assigned = true;
                    --unassigned_occupants_;
                }
                if (state.in_zone || state.side == 1) {
                    set_uncertain("new track appeared after a partial crossing");
                }
                it = tracks_.emplace(track.id, state).first;
            } else {
                it = tracks_.find(track.id);
            }
        }
        if (it == tracks_.end()) continue;

        TrackState& state    = it->second;
        const int   old_side = state.side;
        if (track_trace_) {
            track_trace_(track.id, old_side, new_side, in_zone, initialized_from_first_observation);
        }
        if (old_side == 0 && !in_zone && new_side == 0 && !state.entry_candidate &&
            !state.exit_candidate) {
            new_side = -1;
        }

        if (state.crossing_suppressed) {
            set_uncertain("ambiguous track ID lineage");
            state.side         = new_side;
            state.in_zone      = in_zone;
            state.last_nx      = foot.x;
            state.last_ny      = foot.y;
            state.last_seen_ms = timestamp_ms;
            continue;
        }

        const bool skipped_entry = old_side == -1 && new_side == 1;
        const bool skipped_exit  = old_side == 1 && new_side == -1;
        if (skipped_entry || skipped_exit) {
            const PointF previous_foot{state.last_nx, state.last_ny};
            const PointF previous_relative{previous_foot.x - geometry.centroid.x,
                                           previous_foot.y - geometry.centroid.y};
            const float  previous_projection = dot(previous_relative, geometry.inside_dir);
            const float  displacement =
                skipped_entry ? projection - previous_projection : previous_projection - projection;
            const bool crosses_zone =
                movement_crosses_polygon(previous_foot, foot, geometry.polygon);
            if (crosses_zone && displacement >= geometry.displacement_threshold) {
                if (skipped_entry && !state.occupancy_assigned) {
                    ++count_;
                    state.occupancy_assigned = true;
                    state.occupant_id        = next_occupant_id_++;
                    confidence_              = OccupancyConfidence::Certain;
                    snapshot.events.push_back({OccupancyEventDirection::Entry, track.id,
                                               timestamp_ms, state.occupant_id});
                } else if (skipped_exit && state.occupancy_assigned && count_ > 0) {
                    --count_;
                    const std::uint64_t exiting_occupant = state.occupant_id;
                    state.occupancy_assigned             = false;
                    state.occupant_id                    = 0;
                    confidence_                          = OccupancyConfidence::Certain;
                    snapshot.events.push_back(
                        {OccupancyEventDirection::Exit, track.id, timestamp_ms, exiting_occupant});
                } else {
                    set_uncertain("skipped crossing has no matching occupancy assignment");
                }
            } else {
                set_uncertain("side changed without an observed zone crossing");
            }
            state.entry_candidate = false;
            state.exit_candidate  = false;
            state.side            = new_side;
            state.in_zone         = false;
            state.last_nx         = foot.x;
            state.last_ny         = foot.y;
            state.last_seen_ms    = timestamp_ms;
            continue;
        }

        if (in_zone) {
            if (old_side == -1 && !state.entry_candidate && !state.exit_candidate) {
                state.entry_candidate = true;
                state.start_proj      = projection;
                state.extremum_proj   = projection;
            } else if (old_side == 1 && !state.entry_candidate && !state.exit_candidate) {
                state.exit_candidate = true;
                state.start_proj     = projection;
                state.extremum_proj  = projection;
            } else if (state.entry_candidate) {
                state.extremum_proj = std::max(state.extremum_proj, projection);
            } else if (state.exit_candidate) {
                state.extremum_proj = std::min(state.extremum_proj, projection);
            }
            state.side    = 0;
            state.in_zone = true;
        } else if (new_side == 1) {
            if (state.entry_candidate) {
                const float displacement =
                    std::max(projection, state.extremum_proj) - state.start_proj;
                if (displacement >= geometry.displacement_threshold) {
                    if (!state.occupancy_assigned) {
                        ++count_;
                        state.occupancy_assigned = true;
                        state.occupant_id        = next_occupant_id_++;
                        confidence_              = OccupancyConfidence::Certain;
                        snapshot.events.push_back({OccupancyEventDirection::Entry, track.id,
                                                   timestamp_ms, state.occupant_id});
                    } else {
                        set_uncertain("entry observed for an already assigned track");
                    }
                } else {
                    set_uncertain("aborted entry");
                }
                state.entry_candidate = false;
            } else if (state.exit_candidate) {
                set_uncertain("aborted exit");
                state.exit_candidate = false;
            }
            state.side    = 1;
            state.in_zone = false;
        } else if (new_side == -1) {
            if (state.exit_candidate) {
                const float displacement =
                    state.start_proj - std::min(projection, state.extremum_proj);
                if (displacement >= geometry.displacement_threshold) {
                    if (state.occupancy_assigned && count_ > 0) {
                        --count_;
                        const std::uint64_t exiting_occupant = state.occupant_id;
                        state.occupancy_assigned             = false;
                        state.occupant_id                    = 0;
                        confidence_                          = OccupancyConfidence::Certain;
                        snapshot.events.push_back({OccupancyEventDirection::Exit, track.id,
                                                   timestamp_ms, exiting_occupant});
                    } else {
                        state.occupancy_assigned = false;
                        state.occupant_id        = 0;
                        set_uncertain("exit observed for an unassigned track");
                    }
                } else {
                    set_uncertain("aborted exit");
                }
                state.exit_candidate = false;
            } else if (state.entry_candidate) {
                set_uncertain("aborted entry");
                state.entry_candidate = false;
            }
            state.side    = -1;
            state.in_zone = false;
        }

        state.last_nx      = foot.x;
        state.last_ny      = foot.y;
        state.last_seen_ms = timestamp_ms;
    }

    if (tracking_unreliable) set_uncertain("tracking unreliable / door degraded");
    snapshot.count               = count_;
    snapshot.confidence          = confidence_;
    snapshot.track_occupants     = collect_track_occupants();
    snapshot.pipeline_unreliable = tracking_unreliable || prediction_only_seen;
    notify_if_changed(previous_count, previous_confidence);
    return snapshot;
}

void OccupancyCounter::reconcile(int count) {
    count_                = std::max(0, count);
    unassigned_occupants_ = count_;
    confidence_           = OccupancyConfidence::Certain;
    tracks_.clear();
    ghosts_.clear();
    if (state_change_) state_change_(count_, confidence_);
}

void OccupancyCounter::reconcile_empty() {
    reconcile(0);
}
