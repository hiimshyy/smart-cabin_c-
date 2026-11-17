#include "call_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr std::size_t kMaxOpenCallsPerKind = 64;

double distance_to_stop(const CabinStopRecord& stop, double t_ms) {
    if (t_ms < stop.arrived_at_ms) return stop.arrived_at_ms - t_ms;
    if (stop.closed && t_ms > stop.departed_at_ms) return t_ms - stop.departed_at_ms;
    return 0.0;
}

bool find_matching_stop(const std::deque<CabinStopRecord>& stops, int floor, double cleared_at_ms,
                        double tol_ms, CabinStopRecord& out) {
    bool   found     = false;
    double best_dist = std::numeric_limits<double>::infinity();
    for (auto it = stops.rbegin(); it != stops.rend(); ++it) {
        const CabinStopRecord& stop = *it;
        if (!stop.has_floor || stop.floor != floor) continue;
        if (stop.door_opened_at_ms < 0.0) continue;
        if (cleared_at_ms < stop.arrived_at_ms - tol_ms) continue;
        if (stop.closed && cleared_at_ms > stop.departed_at_ms + tol_ms) continue;
        const double dist = distance_to_stop(stop, cleared_at_ms);
        if (!found || dist < best_dist) {
            out       = stop;
            best_dist = dist;
            found     = true;
            if (dist == 0.0) break;
        }
    }
    return found;
}

void assign_car_origin_stop(const std::deque<CabinStopRecord>& primary_stops,
                            TimelineState tl_state, double now_ms, double tol_ms,
                            CarCallRecord& rec) {
    if (primary_stops.empty()) return;
    if (tl_state == TimelineState::Stopped) {
        for (auto it = primary_stops.rbegin(); it != primary_stops.rend(); ++it) {
            if (!it->closed) {
                rec.registered_stop_key = it->stop_key;
                rec.origin_floor        = it->floor;
                rec.has_origin_floor    = it->has_floor;
                return;
            }
        }
    }
    if (tl_state == TimelineState::Running) {
        for (auto it = primary_stops.rbegin(); it != primary_stops.rend(); ++it) {
            if (it->closed && it->arrived_at_ms <= now_ms) {
                rec.registered_stop_key = it->stop_key;
                rec.origin_floor        = it->floor;
                rec.has_origin_floor    = it->has_floor;
                if (now_ms > it->departed_at_ms) {
                    rec.flags |= TrafficFlag::REGISTERED_WHILE_RUNNING;
                }
                (void)tol_ms;
                return;
            }
        }
    }
}

} // namespace

CallTracker::CallTracker(CallTrackerConfig config) : config_(std::move(config)) {}

std::string CallTracker::next_call_key(const char* kind) {
    return config_.session_hex + "-" + kind + "-" + std::to_string(next_seq_++);
}

void CallTracker::on_auto_call_sent(const std::string& request_id, int floor, double sent_at_ms) {
    if (request_id.empty()) return;
    pending_auto_.push_back(PendingAutoCall{request_id, floor, sent_at_ms});
}

void CallTracker::expire_auto_calls(double now_ms, CallStepResult& out) {
    for (auto it = pending_auto_.begin(); it != pending_auto_.end();) {
        if (now_ms - it->sent_at_ms > config_.call_confirm_ms) {
            out.auto_confirmations.push_back(
                FloorCallConfirmEvent{it->request_id, it->floor, false});
            it = pending_auto_.erase(it);
        } else {
            ++it;
        }
    }
}

void CallTracker::resolve_pending(const std::deque<CabinStopRecord>& primary_stops,
                                  const std::deque<CabinStopRecord>& secondary_stops, double now_ms,
                                  bool force_flush, const char* forced_outcome,
                                  std::uint32_t forced_flag, CallStepResult& out) {
    for (auto it = awaiting_car_.begin(); it != awaiting_car_.end();) {
        CabinStopRecord matched;
        if (find_matching_stop(primary_stops, it->floor, it->cleared_at_ms,
                               config_.floor_tolerance_ms, matched)) {
            it->served_stop_key = matched.stop_key;
            it->outcome         = "served";
            out.completed_car_calls.push_back(std::move(*it));
            it = awaiting_car_.erase(it);
        } else if (now_ms - it->cleared_at_ms > config_.floor_tolerance_ms) {
            it->outcome = "cancelled";
            out.completed_car_calls.push_back(std::move(*it));
            it = awaiting_car_.erase(it);
        } else if (force_flush) {
            it->outcome = forced_outcome;
            it->flags |= forced_flag;
            out.completed_car_calls.push_back(std::move(*it));
            it = awaiting_car_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = awaiting_hall_.begin(); it != awaiting_hall_.end();) {
        CabinStopRecord matched;
        if (find_matching_stop(primary_stops, it->floor, it->cleared_at_ms,
                               config_.floor_tolerance_ms, matched)) {
            it->served_cabin_id   = config_.cabin_id;
            it->has_served_cabin  = true;
            it->served_stop_key   = matched.stop_key;
            it->door_opened_at_ms = matched.door_opened_at_ms;
            if ((it->flags & TrafficFlag::REGISTRATION_UNOBSERVED) == 0 &&
                it->registered_at_ms >= 0.0) {
                it->wait_ms = std::max<std::int64_t>(
                    0, static_cast<std::int64_t>(
                           std::llround(matched.door_opened_at_ms - it->registered_at_ms)));
            }
            it->outcome = "served";
            out.completed_hall_calls.push_back(std::move(*it));
            it = awaiting_hall_.erase(it);
        } else if (now_ms - it->cleared_at_ms > config_.floor_tolerance_ms || force_flush) {
            if (find_matching_stop(secondary_stops, it->floor, it->cleared_at_ms,
                                   config_.floor_tolerance_ms, matched)) {
                it->served_cabin_id   = config_.secondary_slave;
                it->has_served_cabin  = true;
                it->served_stop_key   = matched.stop_key;
                it->door_opened_at_ms = matched.door_opened_at_ms;
                if ((it->flags & TrafficFlag::REGISTRATION_UNOBSERVED) == 0 &&
                    it->registered_at_ms >= 0.0) {
                    it->wait_ms = std::max<std::int64_t>(
                        0, static_cast<std::int64_t>(
                               std::llround(matched.door_opened_at_ms - it->registered_at_ms)));
                }
                it->outcome = "served_by_car1";
            } else if (force_flush) {
                it->outcome = forced_outcome;
                it->flags |= forced_flag;
            } else {
                it->outcome = "cleared_without_stop";
            }
            out.completed_hall_calls.push_back(std::move(*it));
            it = awaiting_hall_.erase(it);
        } else {
            ++it;
        }
    }
}

CallStepResult CallTracker::on_step(const SlaveRegisterState&          primary_state,
                                    TimelineState                      primary_tl_state,
                                    const std::deque<CabinStopRecord>& primary_stops,
                                    const std::deque<CabinStopRecord>& secondary_stops,
                                    bool gap_triggered, double now_ms) {
    CallStepResult out;
    if (gap_triggered || !primary_state.valid) {
        if (mask_initialized_ || !open_car_.empty() || !open_hall_up_.empty() ||
            !open_hall_down_.empty()) {
            for (auto& kv : open_car_) {
                kv.second.cleared_at_ms = now_ms;
                kv.second.outcome       = "register_gap";
                kv.second.flags |= TrafficFlag::REGISTER_GAP;
                out.completed_car_calls.push_back(std::move(kv.second));
            }
            open_car_.clear();
            for (auto& kv : open_hall_up_) {
                kv.second.cleared_at_ms = now_ms;
                kv.second.outcome       = "register_gap";
                kv.second.flags |= TrafficFlag::REGISTER_GAP;
                out.completed_hall_calls.push_back(std::move(kv.second));
            }
            open_hall_up_.clear();
            for (auto& kv : open_hall_down_) {
                kv.second.cleared_at_ms = now_ms;
                kv.second.outcome       = "register_gap";
                kv.second.flags |= TrafficFlag::REGISTER_GAP;
                out.completed_hall_calls.push_back(std::move(kv.second));
            }
            open_hall_down_.clear();
            resolve_pending(primary_stops, secondary_stops, now_ms, true, "register_gap",
                            TrafficFlag::REGISTER_GAP, out);
            mask_initialized_ = false;
            prev_car_mask_    = 0;
            prev_hall_up_     = 0;
            prev_hall_down_   = 0;
        }
        expire_auto_calls(now_ms, out);
        return out;
    }

    int                 invalid_bits = 0;
    const std::uint16_t car_mask =
        filter_floor_mask(primary_state.get(ElevatorReg::CAR_CALLS, 0), config_.floor_min,
                          config_.floor_max, invalid_bits);
    const std::uint16_t hall_up =
        filter_floor_mask(primary_state.get(ElevatorReg::HALL_CALLS_UP, 0), config_.floor_min,
                          config_.floor_max, invalid_bits);
    const std::uint16_t hall_down =
        filter_floor_mask(primary_state.get(ElevatorReg::HALL_CALLS_DOWN, 0), config_.floor_min,
                          config_.floor_max, invalid_bits);
    invalid_mask_bits_count_ += static_cast<std::uint64_t>(invalid_bits);

    if (!mask_initialized_) {
        for (int b = config_.floor_min; b <= config_.floor_max; ++b) {
            if ((car_mask & (1u << b)) != 0 && open_car_.size() < kMaxOpenCallsPerKind) {
                CarCallRecord rec;
                rec.call_key         = next_call_key("c");
                rec.cabin_id         = config_.cabin_id;
                rec.floor            = b;
                rec.registered_at_ms = now_ms;
                rec.source           = "button";
                rec.flags            = TrafficFlag::REGISTRATION_UNOBSERVED;
                assign_car_origin_stop(primary_stops, primary_tl_state, now_ms,
                                       config_.floor_tolerance_ms, rec);
                open_car_.emplace(b, std::move(rec));
            }
            if ((hall_up & (1u << b)) != 0 && open_hall_up_.size() < kMaxOpenCallsPerKind) {
                HallCallRecord rec;
                rec.call_key         = next_call_key("hu");
                rec.floor            = b;
                rec.direction        = "up";
                rec.registered_at_ms = now_ms;
                rec.flags            = TrafficFlag::REGISTRATION_UNOBSERVED;
                open_hall_up_.emplace(b, std::move(rec));
            }
            if ((hall_down & (1u << b)) != 0 && open_hall_down_.size() < kMaxOpenCallsPerKind) {
                HallCallRecord rec;
                rec.call_key         = next_call_key("hd");
                rec.floor            = b;
                rec.direction        = "down";
                rec.registered_at_ms = now_ms;
                rec.flags            = TrafficFlag::REGISTRATION_UNOBSERVED;
                open_hall_down_.emplace(b, std::move(rec));
            }
        }
        prev_car_mask_    = car_mask;
        prev_hall_up_     = hall_up;
        prev_hall_down_   = hall_down;
        mask_initialized_ = true;
        resolve_pending(primary_stops, secondary_stops, now_ms, false, "", 0, out);
        expire_auto_calls(now_ms, out);
        return out;
    }

    for (int b = config_.floor_min; b <= config_.floor_max; ++b) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << b);

        // Car call 0 -> 1 / 1 -> 0
        const bool car_was = (prev_car_mask_ & bit) != 0;
        const bool car_now = (car_mask & bit) != 0;
        if (!car_was && car_now) {
            if (open_car_.find(b) == open_car_.end() && open_car_.size() < kMaxOpenCallsPerKind) {
                CarCallRecord rec;
                rec.call_key         = next_call_key("c");
                rec.cabin_id         = config_.cabin_id;
                rec.floor            = b;
                rec.registered_at_ms = now_ms;
                rec.source           = "button";
                for (auto ait = pending_auto_.begin(); ait != pending_auto_.end(); ++ait) {
                    if (ait->floor == b && now_ms >= ait->sent_at_ms &&
                        now_ms - ait->sent_at_ms <= config_.call_confirm_ms) {
                        rec.source = "auto";
                        out.auto_confirmations.push_back(
                            FloorCallConfirmEvent{ait->request_id, b, true});
                        pending_auto_.erase(ait);
                        break;
                    }
                }
                assign_car_origin_stop(primary_stops, primary_tl_state, now_ms,
                                       config_.floor_tolerance_ms, rec);
                open_car_.emplace(b, std::move(rec));
            }
        } else if (car_was && !car_now) {
            const auto oit = open_car_.find(b);
            if (oit != open_car_.end()) {
                oit->second.cleared_at_ms = now_ms;
                awaiting_car_.push_back(std::move(oit->second));
                open_car_.erase(oit);
            }
        }

        // Hall UP 0 -> 1 / 1 -> 0
        const bool up_was = (prev_hall_up_ & bit) != 0;
        const bool up_now = (hall_up & bit) != 0;
        if (!up_was && up_now) {
            if (open_hall_up_.find(b) == open_hall_up_.end() &&
                open_hall_up_.size() < kMaxOpenCallsPerKind) {
                HallCallRecord rec;
                rec.call_key         = next_call_key("hu");
                rec.floor            = b;
                rec.direction        = "up";
                rec.registered_at_ms = now_ms;
                open_hall_up_.emplace(b, std::move(rec));
            }
        } else if (up_was && !up_now) {
            const auto oit = open_hall_up_.find(b);
            if (oit != open_hall_up_.end()) {
                oit->second.cleared_at_ms = now_ms;
                awaiting_hall_.push_back(std::move(oit->second));
                open_hall_up_.erase(oit);
            }
        }

        // Hall DOWN 0 -> 1 / 1 -> 0
        const bool dn_was = (prev_hall_down_ & bit) != 0;
        const bool dn_now = (hall_down & bit) != 0;
        if (!dn_was && dn_now) {
            if (open_hall_down_.find(b) == open_hall_down_.end() &&
                open_hall_down_.size() < kMaxOpenCallsPerKind) {
                HallCallRecord rec;
                rec.call_key         = next_call_key("hd");
                rec.floor            = b;
                rec.direction        = "down";
                rec.registered_at_ms = now_ms;
                open_hall_down_.emplace(b, std::move(rec));
            }
        } else if (dn_was && !dn_now) {
            const auto oit = open_hall_down_.find(b);
            if (oit != open_hall_down_.end()) {
                oit->second.cleared_at_ms = now_ms;
                awaiting_hall_.push_back(std::move(oit->second));
                open_hall_down_.erase(oit);
            }
        }
    }

    prev_car_mask_  = car_mask;
    prev_hall_up_   = hall_up;
    prev_hall_down_ = hall_down;
    resolve_pending(primary_stops, secondary_stops, now_ms, false, "", 0, out);
    expire_auto_calls(now_ms, out);
    return out;
}

CallStepResult CallTracker::poll(const std::deque<CabinStopRecord>& primary_stops,
                                 const std::deque<CabinStopRecord>& secondary_stops,
                                 double                             now_ms) {
    CallStepResult out;
    resolve_pending(primary_stops, secondary_stops, now_ms, false, "", 0, out);
    expire_auto_calls(now_ms, out);
    return out;
}

CallStepResult CallTracker::shutdown(const std::deque<CabinStopRecord>& primary_stops,
                                     const std::deque<CabinStopRecord>& secondary_stops,
                                     double                             now_ms) {
    CallStepResult out;
    resolve_pending(primary_stops, secondary_stops, now_ms, true, "shutdown", TrafficFlag::SHUTDOWN,
                    out);
    for (auto& kv : open_car_) {
        kv.second.cleared_at_ms = now_ms;
        kv.second.outcome       = "shutdown";
        kv.second.flags |= TrafficFlag::SHUTDOWN;
        out.completed_car_calls.push_back(std::move(kv.second));
    }
    open_car_.clear();
    for (auto& kv : open_hall_up_) {
        kv.second.cleared_at_ms = now_ms;
        kv.second.outcome       = "shutdown";
        kv.second.flags |= TrafficFlag::SHUTDOWN;
        out.completed_hall_calls.push_back(std::move(kv.second));
    }
    open_hall_up_.clear();
    for (auto& kv : open_hall_down_) {
        kv.second.cleared_at_ms = now_ms;
        kv.second.outcome       = "shutdown";
        kv.second.flags |= TrafficFlag::SHUTDOWN;
        out.completed_hall_calls.push_back(std::move(kv.second));
    }
    open_hall_down_.clear();
    for (const auto& p : pending_auto_) {
        out.auto_confirmations.push_back(FloorCallConfirmEvent{p.request_id, p.floor, false});
    }
    pending_auto_.clear();
    return out;
}

std::size_t CallTracker::open_call_count() const {
    return open_car_.size() + open_hall_up_.size() + open_hall_down_.size();
}
