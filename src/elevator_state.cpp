#include "elevator_state.h"

#include "log/logger.h"

#include <utility>

namespace {

std::string make_key(const std::string& session_hex, const char* kind, int slave,
                     std::uint64_t seq) {
    return session_hex + "-" + kind + "-" + std::to_string(slave) + "-" + std::to_string(seq);
}

std::string direction_name(const StatusRegister8480& st) {
    if (st.up && !st.down) return "up";
    if (st.down && !st.up) return "down";
    return "unknown";
}

} // namespace

void RegisterInbox::push(RawRegisterPacket packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
        queue_.pop_front();
        overflow_ = true;
    }
    queue_.push_back(std::move(packet));
}

void RegisterInbox::drain(std::vector<RawRegisterPacket>& out, bool& overflowed) {
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    overflowed = overflow_;
    overflow_  = false;
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
}

bool SlaveRegisterState::has(int reg) const {
    return regs.find(reg) != regs.end();
}

std::uint16_t SlaveRegisterState::get(int reg, std::uint16_t fallback) const {
    const auto it = regs.find(reg);
    return it != regs.end() ? it->second : fallback;
}

CabinTimeline::CabinTimeline(CabinTimelineConfig config) : config_(std::move(config)) {}

void CabinTimeline::accumulate_flags(const SlaveRegisterState& state, std::uint32_t& flags) const {
    if (state.has(ElevatorReg::STATUS)) {
        flags |= status_to_traffic_flags(decode_status_8480(state.get(ElevatorReg::STATUS)));
    }
    if (state.has(ElevatorReg::CTB_INPUT)) {
        if (decode_door_ctb_8471(state.get(ElevatorReg::CTB_INPUT)).overload) {
            flags |= TrafficFlag::OVERLOAD;
        }
    }
}

void CabinTimeline::record_stop_in_history(const CabinStopRecord& stop, double now_ms) {
    for (auto& existing : recent_stops_) {
        if (existing.stop_key == stop.stop_key) {
            existing = stop;
            return;
        }
    }
    recent_stops_.push_back(stop);
    while (!recent_stops_.empty() && recent_stops_.front().closed &&
           now_ms - recent_stops_.front().departed_at_ms > config_.stop_retention_ms &&
           recent_stops_.size() > 64) {
        recent_stops_.pop_front();
    }
}

void CabinTimeline::open_stop(const SlaveRegisterState& state, int floor, bool has_floor,
                              double now_ms, std::vector<TimelineEvent>& events) {
    open_stop_               = CabinStopRecord{};
    open_stop_.stop_key      = make_key(config_.session_hex, "s", config_.slave, next_stop_seq_++);
    open_stop_.cabin_id      = config_.cabin_id;
    open_stop_.slave         = config_.slave;
    open_stop_.floor         = floor;
    open_stop_.has_floor     = has_floor;
    open_stop_.arrived_at_ms = now_ms;
    if (state.has(ElevatorReg::HEIGHT_CM)) {
        open_stop_.height_cm  = static_cast<int>(state.get(ElevatorReg::HEIGHT_CM));
        open_stop_.has_height = true;
    }
    if (state.has(ElevatorReg::CTB_INPUT)) {
        open_stop_.raw_8471_arrive = state.get(ElevatorReg::CTB_INPUT);
        open_stop_.has_8471_arrive = true;
    }
    if (state.has(ElevatorReg::DOOR_OUTPUT)) {
        open_stop_.raw_8472_arrive = state.get(ElevatorReg::DOOR_OUTPUT);
        open_stop_.has_8472_arrive = true;
    }
    if (state.has(ElevatorReg::STATUS)) {
        open_stop_.raw_8480_arrive = state.get(ElevatorReg::STATUS);
        open_stop_.has_8480_arrive = true;
    }
    accumulate_flags(state, open_stop_.flags);

    const bool door_open_now =
        (state.has(ElevatorReg::CTB_INPUT) &&
         decode_door_ctb_8471(state.get(ElevatorReg::CTB_INPUT)).fully_open) ||
        (state.has(ElevatorReg::DOOR_OUTPUT) &&
         decode_door_output_8472(state.get(ElevatorReg::DOOR_OUTPUT)).opening);
    if (door_open_now) open_stop_.door_opened_at_ms = now_ms;

    has_open_stop_ = true;
    state_         = TimelineState::Stopped;
    record_stop_in_history(open_stop_, now_ms);

    TimelineEvent ev;
    ev.type         = TimelineEventType::StopOpened;
    ev.slave        = config_.slave;
    ev.timestamp_ms = now_ms;
    ev.stop         = open_stop_;
    events.push_back(ev);

    if (door_open_now) {
        TimelineEvent door_ev;
        door_ev.type         = TimelineEventType::DoorOpened;
        door_ev.slave        = config_.slave;
        door_ev.timestamp_ms = now_ms;
        door_ev.stop         = open_stop_;
        events.push_back(door_ev);
    }
}

void CabinTimeline::close_stop(const SlaveRegisterState& state, double now_ms,
                               std::uint32_t extra_flags, std::vector<TimelineEvent>& events) {
    if (!has_open_stop_) return;
    open_stop_.departed_at_ms = now_ms;
    open_stop_.closed         = true;
    open_stop_.flags |= extra_flags;
    if (state.has(ElevatorReg::CTB_INPUT)) {
        open_stop_.raw_8471_depart = state.get(ElevatorReg::CTB_INPUT);
        open_stop_.has_8471_depart = true;
    }
    if (state.has(ElevatorReg::DOOR_OUTPUT)) {
        open_stop_.raw_8472_depart = state.get(ElevatorReg::DOOR_OUTPUT);
        open_stop_.has_8472_depart = true;
    }
    if (state.has(ElevatorReg::STATUS)) {
        open_stop_.raw_8480_depart = state.get(ElevatorReg::STATUS);
        open_stop_.has_8480_depart = true;
    }
    accumulate_flags(state, open_stop_.flags);
    record_stop_in_history(open_stop_, now_ms);

    TimelineEvent ev;
    ev.type         = TimelineEventType::StopClosed;
    ev.slave        = config_.slave;
    ev.timestamp_ms = now_ms;
    ev.stop         = open_stop_;
    events.push_back(ev);
    has_open_stop_ = false;
}

void CabinTimeline::open_run(const SlaveRegisterState& state, double now_ms,
                             std::vector<TimelineEvent>& events) {
    open_run_                = CabinRunRecord{};
    open_run_.run_key        = make_key(config_.session_hex, "r", config_.slave, next_run_seq_++);
    open_run_.cabin_id       = config_.cabin_id;
    open_run_.slave          = config_.slave;
    open_run_.departed_at_ms = now_ms;
    if (has_open_stop_) {
        open_run_.from_stop_key  = open_stop_.stop_key;
        open_run_.from_floor     = open_stop_.floor;
        open_run_.has_from_floor = open_stop_.has_floor;
    }
    if (state.has(ElevatorReg::STATUS)) {
        open_run_.direction = direction_name(decode_status_8480(state.get(ElevatorReg::STATUS)));
    }
    if (state.has(ElevatorReg::CAR_CALLS)) {
        int ignored              = 0;
        open_run_.car_calls_mask = filter_floor_mask(state.get(ElevatorReg::CAR_CALLS),
                                                     config_.floor_min, config_.floor_max, ignored);
    }
    accumulate_flags(state, open_run_.flags);
    has_open_run_ = true;
    state_        = TimelineState::Running;
}

void CabinTimeline::close_run(const SlaveRegisterState& state, const std::string& to_stop_key,
                              int to_floor, bool has_to_floor, double now_ms,
                              std::uint32_t extra_flags, std::vector<TimelineEvent>& events,
                              int& run_counter_mismatch_count) {
    if (!has_open_run_) return;
    open_run_.to_stop_key   = to_stop_key;
    open_run_.to_floor      = to_floor;
    open_run_.has_to_floor  = has_to_floor;
    open_run_.arrived_at_ms = now_ms;
    open_run_.closed        = true;
    open_run_.flags |= extra_flags;
    accumulate_flags(state, open_run_.flags);

    if ((extra_flags & (TrafficFlag::REGISTER_GAP | TrafficFlag::SHUTDOWN)) == 0 &&
        state.has(ElevatorReg::RUN_COUNTER_LOW)) {
        const std::uint32_t current_counter = combine_run_counter(
            state.get(ElevatorReg::RUN_COUNTER_LOW), state.get(ElevatorReg::RUN_COUNTER_HIGH, 0));
        if (has_last_run_counter_) {
            const std::uint32_t delta = current_counter - last_run_counter_;
            if (delta != 1u) {
                ++run_counter_mismatch_count;
                LOG_WARN("traffic", "slave=%d run_counter delta=%u (expected 1) prev=%u curr=%u",
                         config_.slave, delta, last_run_counter_, current_counter);
            }
        }
        last_run_counter_     = current_counter;
        has_last_run_counter_ = true;
    }

    TimelineEvent ev;
    ev.type         = TimelineEventType::RunEnded;
    ev.slave        = config_.slave;
    ev.timestamp_ms = now_ms;
    ev.run          = open_run_;
    events.push_back(ev);
    has_open_run_ = false;
}

void CabinTimeline::on_register_state(const SlaveRegisterState& state, double now_ms,
                                      std::vector<TimelineEvent>& events, int& invalid_floor_count,
                                      int& run_counter_mismatch_count) {
    if (!state.valid) return;

    const StatusRegister8480 st      = decode_status_8480(state.get(ElevatorReg::STATUS));
    const bool               running = st.running;

    int        floor_idx = -1;
    const bool has_floor = decode_floor_8465(state.get(ElevatorReg::FLOOR), config_.floor_min,
                                             config_.floor_max, floor_idx);
    if (!has_floor && state.has(ElevatorReg::FLOOR)) {
        ++invalid_floor_count;
    }

    if (state_ == TimelineState::Unknown) {
        if (!running) {
            if (!has_last_run_counter_ && state.has(ElevatorReg::RUN_COUNTER_LOW)) {
                last_run_counter_ =
                    combine_run_counter(state.get(ElevatorReg::RUN_COUNTER_LOW),
                                        state.get(ElevatorReg::RUN_COUNTER_HIGH, 0));
                has_last_run_counter_ = true;
            }
            if (has_floor) {
                open_stop(state, floor_idx, true, now_ms, events);
            }
        } else {
            open_run(state, now_ms, events);
            TimelineEvent ev;
            ev.type         = TimelineEventType::RunStarted;
            ev.slave        = config_.slave;
            ev.timestamp_ms = now_ms;
            ev.run          = open_run_;
            events.push_back(ev);
        }
        return;
    }

    if (state_ == TimelineState::Stopped) {
        if (!running) {
            accumulate_flags(state, open_stop_.flags);
            if (!open_stop_.has_floor && has_floor) {
                open_stop_.floor     = floor_idx;
                open_stop_.has_floor = true;
            }
            const DoorCtb8471    ctb = decode_door_ctb_8471(state.get(ElevatorReg::CTB_INPUT, 0));
            const DoorOutput8472 dout =
                decode_door_output_8472(state.get(ElevatorReg::DOOR_OUTPUT, 0));
            const bool door_open_now   = ctb.fully_open || dout.opening;
            const bool door_closed_now = ctb.fully_closed || dout.closing;
            if (open_stop_.door_opened_at_ms < 0.0 && door_open_now) {
                open_stop_.door_opened_at_ms = now_ms;
                record_stop_in_history(open_stop_, now_ms);
                TimelineEvent door_ev;
                door_ev.type         = TimelineEventType::DoorOpened;
                door_ev.slave        = config_.slave;
                door_ev.timestamp_ms = now_ms;
                door_ev.stop         = open_stop_;
                events.push_back(door_ev);
            } else if (open_stop_.door_opened_at_ms >= 0.0 &&
                       now_ms > open_stop_.door_opened_at_ms &&
                       open_stop_.door_closed_at_ms < 0.0 && door_closed_now) {
                open_stop_.door_closed_at_ms = now_ms;
                record_stop_in_history(open_stop_, now_ms);
                TimelineEvent door_ev;
                door_ev.type         = TimelineEventType::DoorClosed;
                door_ev.slave        = config_.slave;
                door_ev.timestamp_ms = now_ms;
                door_ev.stop         = open_stop_;
                events.push_back(door_ev);
            }
            record_stop_in_history(open_stop_, now_ms);
            return;
        }

        // Transition Stopped -> Running. Capture `open_stop_` metadata before close_stop clears it.
        CabinRunRecord next_run;
        next_run.run_key       = make_key(config_.session_hex, "r", config_.slave, next_run_seq_++);
        next_run.cabin_id      = config_.cabin_id;
        next_run.slave         = config_.slave;
        next_run.from_stop_key = open_stop_.stop_key;
        next_run.from_floor    = open_stop_.floor;
        next_run.has_from_floor = open_stop_.has_floor;
        next_run.direction      = direction_name(st);
        next_run.departed_at_ms = now_ms;
        if (state.has(ElevatorReg::CAR_CALLS)) {
            int ignored             = 0;
            next_run.car_calls_mask = filter_floor_mask(
                state.get(ElevatorReg::CAR_CALLS), config_.floor_min, config_.floor_max, ignored);
        }
        accumulate_flags(state, next_run.flags);

        close_stop(state, now_ms, 0, events);
        open_run_     = std::move(next_run);
        has_open_run_ = true;
        state_        = TimelineState::Running;

        TimelineEvent ev;
        ev.type         = TimelineEventType::RunStarted;
        ev.slave        = config_.slave;
        ev.timestamp_ms = now_ms;
        ev.run          = open_run_;
        events.push_back(ev);
        return;
    }

    // state_ == TimelineState::Running
    if (running) {
        accumulate_flags(state, open_run_.flags);
        if (open_run_.direction == "unknown") {
            const std::string dir = direction_name(st);
            if (dir != "unknown") open_run_.direction = dir;
        }
        return;
    }

    // Transition Running -> Stopped. Open the new stop first to obtain `to_stop_key`, then emit
    // RunEnded followed by StopOpened / DoorOpened? Wait: emit RunEnded BEFORE StopOpened so
    // downstream consumers observe StopClosed -> RunStarted -> RunEnded -> StopOpened cleanly.
    const std::string to_stop_key =
        make_key(config_.session_hex, "s", config_.slave, next_stop_seq_++);
    close_run(state, to_stop_key, floor_idx, has_floor, now_ms, 0, events,
              run_counter_mismatch_count);

    open_stop_               = CabinStopRecord{};
    open_stop_.stop_key      = to_stop_key;
    open_stop_.cabin_id      = config_.cabin_id;
    open_stop_.slave         = config_.slave;
    open_stop_.floor         = floor_idx;
    open_stop_.has_floor     = has_floor;
    open_stop_.arrived_at_ms = now_ms;
    if (state.has(ElevatorReg::HEIGHT_CM)) {
        open_stop_.height_cm  = static_cast<int>(state.get(ElevatorReg::HEIGHT_CM));
        open_stop_.has_height = true;
    }
    if (state.has(ElevatorReg::CTB_INPUT)) {
        open_stop_.raw_8471_arrive = state.get(ElevatorReg::CTB_INPUT);
        open_stop_.has_8471_arrive = true;
    }
    if (state.has(ElevatorReg::DOOR_OUTPUT)) {
        open_stop_.raw_8472_arrive = state.get(ElevatorReg::DOOR_OUTPUT);
        open_stop_.has_8472_arrive = true;
    }
    if (state.has(ElevatorReg::STATUS)) {
        open_stop_.raw_8480_arrive = state.get(ElevatorReg::STATUS);
        open_stop_.has_8480_arrive = true;
    }
    accumulate_flags(state, open_stop_.flags);

    const bool door_open_now =
        (state.has(ElevatorReg::CTB_INPUT) &&
         decode_door_ctb_8471(state.get(ElevatorReg::CTB_INPUT)).fully_open) ||
        (state.has(ElevatorReg::DOOR_OUTPUT) &&
         decode_door_output_8472(state.get(ElevatorReg::DOOR_OUTPUT)).opening);
    if (door_open_now) open_stop_.door_opened_at_ms = now_ms;

    has_open_stop_ = true;
    state_         = TimelineState::Stopped;
    record_stop_in_history(open_stop_, now_ms);

    TimelineEvent ev;
    ev.type         = TimelineEventType::StopOpened;
    ev.slave        = config_.slave;
    ev.timestamp_ms = now_ms;
    ev.stop         = open_stop_;
    events.push_back(ev);

    if (door_open_now) {
        TimelineEvent door_ev;
        door_ev.type         = TimelineEventType::DoorOpened;
        door_ev.slave        = config_.slave;
        door_ev.timestamp_ms = now_ms;
        door_ev.stop         = open_stop_;
        events.push_back(door_ev);
    }
}

void CabinTimeline::on_register_gap(double now_ms, std::vector<TimelineEvent>& events) {
    const SlaveRegisterState empty_state;
    int                      dummy_mismatch = 0;
    if (has_open_stop_) close_stop(empty_state, now_ms, TrafficFlag::REGISTER_GAP, events);
    if (has_open_run_) {
        close_run(empty_state, "", -1, false, now_ms, TrafficFlag::REGISTER_GAP, events,
                  dummy_mismatch);
    }
    state_                = TimelineState::Unknown;
    has_last_run_counter_ = false;

    TimelineEvent ev;
    ev.type         = TimelineEventType::RegisterGap;
    ev.slave        = config_.slave;
    ev.timestamp_ms = now_ms;
    events.push_back(ev);
}

void CabinTimeline::on_shutdown(double now_ms, std::vector<TimelineEvent>& events) {
    const SlaveRegisterState empty_state;
    int                      dummy_mismatch = 0;
    if (has_open_stop_) close_stop(empty_state, now_ms, TrafficFlag::SHUTDOWN, events);
    if (has_open_run_) {
        close_run(empty_state, "", -1, false, now_ms, TrafficFlag::SHUTDOWN, events,
                  dummy_mismatch);
    }
    state_ = TimelineState::Unknown;
}

ElevatorStateTracker::ElevatorStateTracker(ElevatorStateConfig config)
    : config_(std::move(config)),
      primary_timeline_(CabinTimelineConfig{config_.cabin_id, config_.primary_slave,
                                            config_.floor_min, config_.floor_max,
                                            config_.session_hex}),
      secondary_timeline_(CabinTimelineConfig{config_.cabin_id, config_.secondary_slave,
                                              config_.floor_min, config_.floor_max,
                                              config_.session_hex}) {
    primary_state_.slave   = config_.primary_slave;
    secondary_state_.slave = config_.secondary_slave;
}

bool ElevatorStateTracker::is_slave_complete(const SlaveRegisterState& state) const {
    return state.has(ElevatorReg::FLOOR) && state.has(ElevatorReg::STATUS) &&
           state.has(ElevatorReg::CAR_CALLS) && state.has(ElevatorReg::HALL_CALLS_UP) &&
           state.has(ElevatorReg::HALL_CALLS_DOWN) &&
           (state.has(ElevatorReg::CTB_INPUT) || state.has(ElevatorReg::DOOR_OUTPUT));
}

void ElevatorStateTracker::trigger_gap(double now_ms, ElevatorStepResult& result) {
    result.gap_triggered = true;
    primary_state_.valid = false;
    primary_state_.regs.clear();
    secondary_state_.valid = false;
    secondary_state_.regs.clear();
    primary_timeline_.on_register_gap(now_ms, result.primary_events);
    secondary_timeline_.on_register_gap(now_ms, result.secondary_events);
}

ElevatorStepResult ElevatorStateTracker::apply_update(const RegisterUpdate& update) {
    ElevatorStepResult result;
    if (update.invalid_values > 0) {
        register_invalid_count_ += static_cast<std::uint64_t>(update.invalid_values);
    }

    if (active_session_ != 0 && update.session != 0 && update.session != active_session_) {
        trigger_gap(update.received_ms, result);
    }
    if (update.session != 0) active_session_ = update.session;
    last_any_message_ms_ = update.received_ms;

    bool primary_touched   = (update.addr_slave == config_.primary_slave);
    bool secondary_touched = (update.addr_slave == config_.secondary_slave);

    for (const RegisterValueEntry& entry : update.values) {
        if (entry.slave == config_.primary_slave) {
            primary_state_.regs[entry.reg] = entry.value;
            primary_touched                = true;
        } else if (entry.slave == config_.secondary_slave) {
            secondary_state_.regs[entry.reg] = entry.value;
            secondary_touched                = true;
        }
    }

    if (primary_touched) {
        primary_state_.session         = active_session_;
        primary_state_.last_message_ms = update.received_ms;
        if (!primary_state_.valid && is_slave_complete(primary_state_)) {
            primary_state_.valid = true;
        }
        if (primary_state_.valid) {
            int invalid_floor = 0;
            int mismatch      = 0;
            primary_timeline_.on_register_state(primary_state_, update.received_ms,
                                                result.primary_events, invalid_floor, mismatch);
            register_invalid_count_ += static_cast<std::uint64_t>(invalid_floor);
            run_counter_mismatch_count_ += static_cast<std::uint64_t>(mismatch);
        }
    }

    if (secondary_touched) {
        secondary_state_.session         = active_session_;
        secondary_state_.last_message_ms = update.received_ms;
        if (!secondary_state_.valid && is_slave_complete(secondary_state_)) {
            secondary_state_.valid = true;
        }
        if (secondary_state_.valid) {
            int invalid_floor = 0;
            int mismatch      = 0;
            secondary_timeline_.on_register_state(secondary_state_, update.received_ms,
                                                  result.secondary_events, invalid_floor, mismatch);
            register_invalid_count_ += static_cast<std::uint64_t>(invalid_floor);
            run_counter_mismatch_count_ += static_cast<std::uint64_t>(mismatch);
        }
    }

    return result;
}

ElevatorStepResult ElevatorStateTracker::poll(double now_ms, bool socket_connected,
                                              std::uint64_t socket_session, bool inbox_overflow) {
    ElevatorStepResult result;
    const bool         disconnected = was_connected_ && !socket_connected;
    const bool         session_changed =
        active_session_ != 0 && socket_session != 0 && socket_session != active_session_;
    was_connected_ = socket_connected;

    if (disconnected || session_changed || inbox_overflow) {
        if (socket_session != 0) active_session_ = socket_session;
        trigger_gap(now_ms, result);
        return result;
    }

    if (primary_timeline_.state() == TimelineState::Running &&
        primary_state_.last_message_ms >= 0.0 &&
        now_ms - primary_state_.last_message_ms > config_.run_timeout_ms) {
        result.gap_triggered = true;
        primary_state_.valid = false;
        primary_state_.regs.clear();
        primary_timeline_.on_register_gap(now_ms, result.primary_events);
    }

    if (secondary_timeline_.state() == TimelineState::Running &&
        secondary_state_.last_message_ms >= 0.0 &&
        now_ms - secondary_state_.last_message_ms > config_.run_timeout_ms) {
        result.gap_triggered   = true;
        secondary_state_.valid = false;
        secondary_state_.regs.clear();
        secondary_timeline_.on_register_gap(now_ms, result.secondary_events);
    }

    return result;
}

ElevatorStepResult ElevatorStateTracker::shutdown(double now_ms) {
    ElevatorStepResult result;
    primary_timeline_.on_shutdown(now_ms, result.primary_events);
    secondary_timeline_.on_shutdown(now_ms, result.secondary_events);
    return result;
}
