#include "floor_call_coordinator.h"

#include <utility>

const char* floor_call_state_name(FloorCallState state) {
    switch (state) {
    case FloorCallState::Queued:
        return "QUEUED";
    case FloorCallState::Sent:
        return "SENT";
    case FloorCallState::AckedSuccess:
        return "ACKED_SUCCESS";
    case FloorCallState::AckedDuplicate:
        return "ACKED_DUPLICATE";
    case FloorCallState::AckedError:
        return "ACKED_ERROR";
    case FloorCallState::Expired:
        return "EXPIRED";
    case FloorCallState::Ambiguous:
        return "AMBIGUOUS";
    case FloorCallState::Suppressed:
        return "SUPPRESSED";
    case FloorCallState::Rejected:
        return "REJECTED";
    }
    return "?";
}

void CabinFloorTracker::reset() {
    has_current_floor_ = false;
    current_floor_     = -1;
    status_up_         = false;
    status_down_       = false;
    hall_up_mask_      = 0;
    hall_down_mask_    = 0;
    last_hall_up_ms_.clear();
    last_hall_down_ms_.clear();
}

void CabinFloorTracker::update(bool has_floor, int floor_index, bool status_up, bool status_down,
                               std::uint16_t hall_up_mask, bool has_hall_up,
                               std::uint16_t hall_down_mask, bool has_hall_down, double now_ms) {
    has_current_floor_ = has_floor;
    current_floor_     = has_floor ? floor_index : -1;
    status_up_         = status_up;
    status_down_       = status_down;
    auto record_mask   = [&](std::uint16_t mask, std::uint16_t& stored,
                           std::map<int, double>& timestamps) {
        stored = mask;
        for (int bit = 0; bit < 16; ++bit) {
            if ((mask & (1u << bit)) != 0) timestamps[bit] = now_ms;
        }
    };
    if (has_hall_up) record_mask(hall_up_mask, hall_up_mask_, last_hall_up_ms_);
    if (has_hall_down) record_mask(hall_down_mask, hall_down_mask_, last_hall_down_ms_);
}

CabinFloorContext CabinFloorTracker::snapshot(double now_ms) const {
    CabinFloorContext ctx;
    if (!has_current_floor_ || current_floor_ < 0 || current_floor_ >= 16) return ctx;
    ctx.has_current_floor = true;
    ctx.current_floor     = current_floor_;

    auto hall_recent = [&](std::uint16_t mask, const std::map<int, double>& times,
                           double& out_ts) -> bool {
        if ((mask & (1u << current_floor_)) != 0) {
            const auto it = times.find(current_floor_);
            out_ts        = (it != times.end()) ? it->second : now_ms;
            return true;
        }
        const auto it = times.find(current_floor_);
        if (it != times.end() && now_ms - it->second <= kHallCallMemoryMs) {
            out_ts = it->second;
            return true;
        }
        return false;
    };

    double     up_ts = -1.0, down_ts = -1.0;
    const bool up_recent   = hall_recent(hall_up_mask_, last_hall_up_ms_, up_ts);
    const bool down_recent = hall_recent(hall_down_mask_, last_hall_down_ms_, down_ts);

    // Prefer hall calls (including short-term clear memory) over 8480 status
    // because 8480 can lag or retain the previous run direction when stopped.
    if (down_recent && !up_recent) {
        ctx.direction = CabinTravelDirection::Down;
    } else if (up_recent && !down_recent) {
        ctx.direction = CabinTravelDirection::Up;
    } else if (down_recent && up_recent && down_ts != up_ts) {
        ctx.direction = (down_ts > up_ts) ? CabinTravelDirection::Down : CabinTravelDirection::Up;
    } else if (status_down_ && !status_up_) {
        ctx.direction = CabinTravelDirection::Down;
    } else if (status_up_ && !status_down_) {
        ctx.direction = CabinTravelDirection::Up;
    } else if (down_recent && up_recent) {
        ctx.direction = (down_ts >= up_ts) ? CabinTravelDirection::Down : CabinTravelDirection::Up;
    }
    return ctx;
}

FloorCallCoordinator::FloorCallCoordinator(FloorCallConfig config, EnqueueFn enqueue)
    : config_(std::move(config)), enqueue_(std::move(enqueue)) {}

void FloorCallCoordinator::set_config(const FloorCallConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void FloorCallCoordinator::set_enqueue(EnqueueFn enqueue) {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueue_ = std::move(enqueue);
}

void FloorCallCoordinator::set_decision_callback(DecisionFn callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    decision_callback_ = std::move(callback);
}

bool FloorCallCoordinator::terminal(FloorCallState state) {
    return state == FloorCallState::AckedSuccess || state == FloorCallState::AckedDuplicate ||
           state == FloorCallState::AckedError || state == FloorCallState::Expired ||
           state == FloorCallState::Ambiguous || state == FloorCallState::Suppressed ||
           state == FloorCallState::Rejected;
}

bool FloorCallCoordinator::encode_floor(int floor, std::int64_t& value) {
    if (floor < 0 || floor > 62) return false;
    value = std::int64_t{1} << floor;
    return true;
}

FloorSelectionResult FloorCallCoordinator::select_target_floor(int home_floor, int secondary_floor,
                                                               const CabinFloorContext& context,
                                                               int floors_min) {
    const bool has_home = (home_floor >= 0);
    const bool has_sec  = (secondary_floor >= 0);
    if (!has_home && !has_sec) return {-1, "no_floor"};

    if (!context.has_current_floor || context.current_floor < 0) {
        return has_home ? FloorSelectionResult{home_floor, ""}
                        : FloorSelectionResult{-1, "no_floor"};
    }

    const int  cur       = context.current_floor;
    const auto dir       = context.direction;
    const bool at_bottom = (cur <= floors_min);

    // Single-floor resident (only home_floor, only secondary_floor, or equal values).
    if (!has_home || !has_sec || home_floor == secondary_floor) {
        const int only_floor = has_home ? home_floor : secondary_floor;
        if (cur == only_floor) return {-1, "already_at_floor"};
        if (cur < only_floor) {
            if (at_bottom || dir != CabinTravelDirection::Down) return {only_floor, ""};
            return {-1, "direction_mismatch"};
        }
        // cur > only_floor
        if (dir != CabinTravelDirection::Up) return {only_floor, ""};
        return {-1, "direction_mismatch"};
    }

    // Two distinct registered floors: symmetric handling for both
    // secondary < home (typical) and secondary > home.
    const int low     = (home_floor < secondary_floor) ? home_floor : secondary_floor;
    const int high    = (home_floor > secondary_floor) ? home_floor : secondary_floor;
    const int primary = home_floor;

    if (cur < low) {
        if (at_bottom || dir != CabinTravelDirection::Down) return {primary, ""};
        return {-1, "direction_mismatch"};
    }
    if (cur == low) {
        if (dir == CabinTravelDirection::Up) return {high, ""};
        if (dir == CabinTravelDirection::Down) {
            return at_bottom ? FloorSelectionResult{high, ""}
                             : FloorSelectionResult{-1, "direction_mismatch"};
        }
        return (cur == primary) ? FloorSelectionResult{-1, "already_at_floor"}
                                : FloorSelectionResult{high, ""};
    }
    if (cur < high) {
        // Intermediate floor: low < cur < high.
        if (dir == CabinTravelDirection::Up) return {high, ""};
        if (dir == CabinTravelDirection::Down) return {low, ""};
        return {primary, ""};
    }
    if (cur == high) {
        if (dir == CabinTravelDirection::Down) return {low, ""};
        if (dir == CabinTravelDirection::Up) return {-1, "direction_mismatch"};
        return (cur == primary) ? FloorSelectionResult{-1, "already_at_floor"}
                                : FloorSelectionResult{low, ""};
    }
    // cur > high
    if (dir != CabinTravelDirection::Up) return {primary, ""};
    return {-1, "direction_mismatch"};
}

void FloorCallCoordinator::start_visit_locked(double now_ms) {
    if (visit_id_ != 0) return;
    visit_id_       = next_visit_id_++;
    next_sequence_  = 1;
    empty_since_ms_ = -1.0;
    resident_decisions_.clear();
    floor_request_ids_.clear();
    commands_.clear();
}

void FloorCallCoordinator::finish_visit_locked(double /*now_ms*/) {
    visit_id_       = 0;
    next_sequence_  = 1;
    empty_since_ms_ = -1.0;
    resident_decisions_.clear();
    floor_request_ids_.clear();
    commands_.clear();
}

FloorCallDecision FloorCallCoordinator::make_decision_locked(std::int64_t resident_id, int floor,
                                                             FloorCallState     state,
                                                             const std::string& reason,
                                                             const std::string& request_id) const {
    FloorCallDecision decision;
    decision.visit_id    = visit_id_;
    decision.request_id  = request_id;
    decision.resident_id = resident_id;
    decision.floor       = floor;
    decision.state       = state;
    decision.reason      = reason;
    return decision;
}

void FloorCallCoordinator::dispatch(FloorCallDecision decision, DecisionFn callback) const {
    if (callback) callback(decision);
}

void FloorCallCoordinator::on_presence(const std::vector<int>& active_subject_keys, double now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_subject_keys.empty()) {
        start_visit_locked(now_ms);
        empty_since_ms_ = -1.0;
        return;
    }
    if (visit_id_ == 0) return;
    if (empty_since_ms_ < 0.0) {
        empty_since_ms_ = now_ms;
        return;
    }
    if (now_ms - empty_since_ms_ >= config_.session_empty_ms) finish_visit_locked(now_ms);
}

FloorCallDecision FloorCallCoordinator::on_confirmed(int subject_key, std::int64_t resident_id,
                                                     int home_floor, float similarity,
                                                     double now_ms) {
    return on_confirmed(subject_key, resident_id, home_floor, -1, CabinFloorContext{}, similarity,
                        now_ms);
}

FloorCallDecision FloorCallCoordinator::on_confirmed(int subject_key, std::int64_t resident_id,
                                                     int home_floor, int secondary_floor,
                                                     const CabinFloorContext& context,
                                                     float similarity, double now_ms) {
    FloorCallDecision decision;
    DecisionFn        callback;
    EnqueueFn         enqueue;
    FloorCallCommand  command;
    bool              should_enqueue = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        start_visit_locked(now_ms); // supports SCRFD-only mode without a person tracker
        callback = decision_callback_;

        const auto already = resident_decisions_.find(resident_id);
        if (already != resident_decisions_.end()) {
            decision = make_decision_locked(resident_id, already->second.floor,
                                            FloorCallState::Suppressed, "same_resident");
        } else if (resident_id < 0) {
            decision =
                make_decision_locked(resident_id, -1, FloorCallState::Rejected, "invalid_resident");
        } else {
            const FloorSelectionResult sel =
                select_target_floor(home_floor, secondary_floor, context, config_.floors_min);
            if (sel.target_floor < 0) {
                decision =
                    make_decision_locked(resident_id, -1, FloorCallState::Suppressed, sel.reason);
            } else {
                const int target_floor = sel.target_floor;
                if (target_floor < config_.floors_min || target_floor > config_.floors_max) {
                    decision =
                        make_decision_locked(resident_id, target_floor, FloorCallState::Rejected,
                                             "floor_out_of_cabin_range");
                } else {
                    std::int64_t value = 0;
                    if (!encode_floor(target_floor, value)) {
                        decision =
                            make_decision_locked(resident_id, target_floor,
                                                 FloorCallState::Rejected, "floor_not_encodable");
                    } else {
                        const auto same_floor = floor_request_ids_.find(target_floor);
                        if (same_floor != floor_request_ids_.end()) {
                            decision = make_decision_locked(resident_id, target_floor,
                                                            FloorCallState::Suppressed,
                                                            "same_floor", same_floor->second);
                        } else if (static_cast<int>(commands_.size()) >=
                                   config_.max_commands_per_visit) {
                            decision = make_decision_locked(resident_id, target_floor,
                                                            FloorCallState::Rejected,
                                                            "visit_command_limit");
                        } else {
                            command.visit_id   = visit_id_;
                            command.sequence   = next_sequence_++;
                            command.request_id = config_.request_id_prefix + ":" +
                                                 std::to_string(config_.cabin_id) + ":" +
                                                 std::to_string(visit_id_) + ":" +
                                                 std::to_string(command.sequence);
                            command.resident_id              = resident_id;
                            command.subject_key              = subject_key;
                            command.floor                    = target_floor;
                            command.value                    = value;
                            command.similarity               = similarity;
                            command.created_at_ms            = now_ms;
                            floor_request_ids_[target_floor] = command.request_id;
                            commands_[command.request_id] =
                                CommandRecord{command, FloorCallState::Queued};
                            decision       = make_decision_locked(resident_id, target_floor,
                                                                  FloorCallState::Queued, "",
                                                                  command.request_id);
                            should_enqueue = true;
                            enqueue        = enqueue_;
                        }
                    }
                }
            }
        }
        resident_decisions_[resident_id] = decision;
    }

    if (!should_enqueue) {
        dispatch(decision, callback);
        return decision;
    }

    // The queue operation must be non-blocking.  Do it after releasing the
    // coordinator mutex so a transport callback can never deadlock this path.
    const bool queued = enqueue && enqueue(command);
    if (!queued) {
        on_transport_state(command.request_id, FloorCallState::Rejected, "transport_queue_full");
        decision.state  = FloorCallState::Rejected;
        decision.reason = "transport_queue_full";
    } else {
        dispatch(decision, callback);
    }
    return decision;
}

void FloorCallCoordinator::on_transport_state(const std::string& request_id, FloorCallState state,
                                              const std::string& reason) {
    FloorCallDecision decision;
    DecisionFn        callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = commands_.find(request_id);
        if (it == commands_.end()) return; // result of a completed/old visit
        if (terminal(it->second.state)) return;
        it->second.state = state;
        // A rejected transport queue has definitely not sent the command, so
        // another resident for this floor may still make a fresh request.
        if (state == FloorCallState::Rejected && reason == "transport_queue_full") {
            const auto floor_it = floor_request_ids_.find(it->second.command.floor);
            if (floor_it != floor_request_ids_.end() && floor_it->second == request_id)
                floor_request_ids_.erase(floor_it);
        }
        decision = make_decision_locked(it->second.command.resident_id, it->second.command.floor,
                                        state, reason, request_id);
        callback = decision_callback_;
    }
    dispatch(decision, callback);
}

std::uint64_t FloorCallCoordinator::active_visit_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visit_id_;
}

size_t FloorCallCoordinator::command_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commands_.size();
}
