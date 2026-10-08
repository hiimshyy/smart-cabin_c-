#include "floor_call_coordinator.h"

#include <utility>

const char* floor_call_state_name(FloorCallState state) {
    switch (state) {
        case FloorCallState::Queued:         return "QUEUED";
        case FloorCallState::Sent:           return "SENT";
        case FloorCallState::AckedSuccess:   return "ACKED_SUCCESS";
        case FloorCallState::AckedDuplicate: return "ACKED_DUPLICATE";
        case FloorCallState::AckedError:     return "ACKED_ERROR";
        case FloorCallState::Expired:        return "EXPIRED";
        case FloorCallState::Ambiguous:      return "AMBIGUOUS";
        case FloorCallState::Suppressed:     return "SUPPRESSED";
        case FloorCallState::Rejected:       return "REJECTED";
    }
    return "?";
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
    if (floor <= 0 || floor > 62) return false;
    value = std::int64_t{1} << floor;
    return true;
}

void FloorCallCoordinator::start_visit_locked(double now_ms) {
    if (visit_id_ != 0) return;
    visit_id_ = next_visit_id_++;
    next_sequence_ = 1;
    empty_since_ms_ = -1.0;
    resident_decisions_.clear();
    floor_request_ids_.clear();
    commands_.clear();
}

void FloorCallCoordinator::finish_visit_locked(double /*now_ms*/) {
    visit_id_ = 0;
    next_sequence_ = 1;
    empty_since_ms_ = -1.0;
    resident_decisions_.clear();
    floor_request_ids_.clear();
    commands_.clear();
}

FloorCallDecision FloorCallCoordinator::make_decision_locked(
    std::int64_t resident_id, int floor, FloorCallState state,
    const std::string& reason, const std::string& request_id) const {
    FloorCallDecision decision;
    decision.visit_id = visit_id_;
    decision.request_id = request_id;
    decision.resident_id = resident_id;
    decision.floor = floor;
    decision.state = state;
    decision.reason = reason;
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

FloorCallDecision FloorCallCoordinator::on_confirmed(
    int subject_key, std::int64_t resident_id, int home_floor, float similarity, double now_ms) {
    FloorCallDecision decision;
    DecisionFn callback;
    EnqueueFn enqueue;
    FloorCallCommand command;
    bool should_enqueue = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        start_visit_locked(now_ms);  // supports SCRFD-only mode without a person tracker
        callback = decision_callback_;

        const auto already = resident_decisions_.find(resident_id);
        if (already != resident_decisions_.end()) {
            decision = make_decision_locked(resident_id, home_floor, FloorCallState::Suppressed,
                                            "same_resident");
        } else if (resident_id < 0) {
            decision = make_decision_locked(resident_id, home_floor, FloorCallState::Rejected,
                                            "invalid_resident");
        } else if (home_floor <= 0) {
            decision = make_decision_locked(resident_id, home_floor, FloorCallState::Suppressed,
                                            "no_floor");
        } else if (home_floor < config_.floors_min || home_floor > config_.floors_max) {
            decision = make_decision_locked(resident_id, home_floor, FloorCallState::Rejected,
                                            "floor_out_of_cabin_range");
        } else {
            std::int64_t value = 0;
            if (!encode_floor(home_floor, value)) {
                decision = make_decision_locked(resident_id, home_floor, FloorCallState::Rejected,
                                                "floor_not_encodable");
            } else {
                const auto same_floor = floor_request_ids_.find(home_floor);
                if (same_floor != floor_request_ids_.end()) {
                    decision = make_decision_locked(resident_id, home_floor, FloorCallState::Suppressed,
                                                    "same_floor", same_floor->second);
                } else if (static_cast<int>(commands_.size()) >= config_.max_commands_per_visit) {
                    decision = make_decision_locked(resident_id, home_floor, FloorCallState::Rejected,
                                                    "visit_command_limit");
                } else {
                    command.visit_id = visit_id_;
                    command.sequence = next_sequence_++;
                    command.request_id = config_.request_id_prefix + ":" +
                                         std::to_string(config_.cabin_id) + ":" +
                                         std::to_string(visit_id_) + ":" +
                                         std::to_string(command.sequence);
                    command.resident_id = resident_id;
                    command.subject_key = subject_key;
                    command.floor = home_floor;
                    command.value = value;
                    command.similarity = similarity;
                    command.created_at_ms = now_ms;
                    floor_request_ids_[home_floor] = command.request_id;
                    commands_[command.request_id] = CommandRecord{command, FloorCallState::Queued};
                    decision = make_decision_locked(resident_id, home_floor, FloorCallState::Queued,
                                                    "", command.request_id);
                    should_enqueue = true;
                    enqueue = enqueue_;
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
        decision.state = FloorCallState::Rejected;
        decision.reason = "transport_queue_full";
    } else {
        dispatch(decision, callback);
    }
    return decision;
}

void FloorCallCoordinator::on_transport_state(const std::string& request_id,
                                               FloorCallState state,
                                               const std::string& reason) {
    FloorCallDecision decision;
    DecisionFn callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = commands_.find(request_id);
        if (it == commands_.end()) return;  // result of a completed/old visit
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
