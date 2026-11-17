#include "trip_assembler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace {

constexpr std::size_t kMaxOpenTrips           = 64;
constexpr std::size_t kMaxPendingTracks       = 32;
constexpr std::size_t kMaxRecentCallHistory   = 128;
constexpr double      kCallHistoryRetentionMs = 600000.0;

std::string make_trip_key(const std::string& session_hex, int slave, std::uint64_t seq) {
    return session_hex + "-t-" + std::to_string(slave) + "-" + std::to_string(seq);
}

double distance_to_stop(const CabinStopRecord& stop, double t_ms) {
    if (t_ms < stop.arrived_at_ms) return stop.arrived_at_ms - t_ms;
    if (stop.closed && t_ms > stop.departed_at_ms) return t_ms - stop.departed_at_ms;
    return 0.0;
}

bool stop_contains_range(const CabinStopRecord& stop, double t_ms, double tol_ms) {
    if (t_ms < stop.arrived_at_ms - tol_ms) return false;
    if (stop.closed && t_ms > stop.departed_at_ms + tol_ms) return false;
    return true;
}

std::string compute_confidence(std::uint32_t flags) {
    constexpr std::uint32_t kLowConfidenceMask =
        TrafficFlag::OCCUPANCY_UNCERTAIN_IN | TrafficFlag::OCCUPANCY_UNCERTAIN_OUT |
        TrafficFlag::ENTRY_UNOBSERVED | TrafficFlag::EXIT_UNOBSERVED | TrafficFlag::FLOOR_UNKNOWN |
        TrafficFlag::REGISTER_GAP;
    return (flags & kLowConfidenceMask) == 0 ? "high" : "low";
}

} // namespace

TripAssembler::TripAssembler(TripAssemblerConfig config) : config_(std::move(config)) {}

std::string TripAssembler::next_trip_key() {
    return make_trip_key(config_.session_hex, config_.slave, next_trip_seq_++);
}

void TripAssembler::prune_pending(double now_ms) {
    for (auto it = pending_by_track_.begin(); it != pending_by_track_.end();) {
        if (now_ms - it->second.last_updated_ms > config_.pending_identity_ttl_ms) {
            it = pending_by_track_.erase(it);
        } else {
            ++it;
        }
    }
    while (pending_by_track_.size() > kMaxPendingTracks) {
        auto oldest = pending_by_track_.begin();
        for (auto it = pending_by_track_.begin(); it != pending_by_track_.end(); ++it) {
            if (it->second.last_updated_ms < oldest->second.last_updated_ms) {
                oldest = it;
            }
        }
        pending_by_track_.erase(oldest);
    }
}

void TripAssembler::add_sample_to_list(std::vector<FaceSample>& list, FaceSample sample) const {
    const std::size_t max_samples =
        static_cast<std::size_t>(std::max(1, config_.max_face_samples_per_trip));
    const bool keep_crops = (config_.save_faces != TrafficSaveFaces::Off);
    if (!keep_crops) sample.crop_bgr_112.clear();

    list.push_back(std::move(sample));
    std::stable_sort(list.begin(), list.end(), [](const FaceSample& a, const FaceSample& b) {
        return a.quality > b.quality;
    });
    if (list.size() > max_samples) {
        list.resize(max_samples);
    }

    // Only keep crop bytes on the highest-quality sample to bound memory.
    if (keep_crops && list.size() > 1) {
        std::size_t best_crop_idx = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!list[i].crop_bgr_112.empty()) {
                best_crop_idx = i;
                break;
            }
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i != best_crop_idx) {
                list[i].crop_bgr_112.clear();
                list[i].crop_w = 0;
                list[i].crop_h = 0;
            }
        }
    }
}

float TripAssembler::best_crop_quality_in(const std::vector<FaceSample>& list) const {
    float best = -1.0f;
    for (const auto& s : list) {
        if (!s.crop_bgr_112.empty() && s.quality > best) {
            best = s.quality;
        }
    }
    return best;
}

bool TripAssembler::wants_crop_for_track(int track_id, float quality) const {
    if (config_.save_faces == TrafficSaveFaces::Off) return false;
    const auto oit = track_to_occupant_.find(track_id);
    if (oit != track_to_occupant_.end()) {
        const auto tit = open_trips_.find(oit->second);
        if (tit != open_trips_.end()) {
            return quality > best_crop_quality_in(tit->second.record.face_samples);
        }
    }
    const auto pit = pending_by_track_.find(track_id);
    if (pit != pending_by_track_.end()) {
        return quality > best_crop_quality_in(pit->second.samples);
    }
    return true;
}

void TripAssembler::on_resident_confirmed(int track_id, std::int64_t resident_id, float similarity,
                                          double now_ms) {
    if (track_id < 0 || resident_id < 0) return;
    ResidentConfirmation conf{resident_id, similarity, now_ms};
    const auto           oit = track_to_occupant_.find(track_id);
    if (oit != track_to_occupant_.end()) {
        const auto tit = open_trips_.find(oit->second);
        if (tit != open_trips_.end()) {
            tit->second.confirmations.push_back(conf);
            return;
        }
    }
    PendingIdentityBucket& bucket = pending_by_track_[track_id];
    bucket.track_id               = track_id;
    bucket.last_updated_ms        = now_ms;
    bucket.confirmations.push_back(conf);
    prune_pending(now_ms);
}

void TripAssembler::on_face_sample(int track_id, FaceSample sample, double now_ms) {
    if (track_id < 0 || sample.embedding.empty()) return;
    sample.timestamp_ms = now_ms;

    auto update_suggestion = [&](std::int64_t& best_rid, float& best_sim) {
        if (sample.best_resident_id >= 0 && sample.best_resident_sim >= config_.suggest_min_sim &&
            sample.best_resident_sim > best_sim) {
            best_rid = sample.best_resident_id;
            best_sim = sample.best_resident_sim;
        }
    };

    const auto oit = track_to_occupant_.find(track_id);
    if (oit != track_to_occupant_.end()) {
        const auto tit = open_trips_.find(oit->second);
        if (tit != open_trips_.end()) {
            update_suggestion(tit->second.record.suggested_resident_id,
                              tit->second.record.suggested_resident_sim);
            add_sample_to_list(tit->second.record.face_samples, std::move(sample));
            return;
        }
    }

    PendingIdentityBucket& bucket = pending_by_track_[track_id];
    bucket.track_id               = track_id;
    bucket.last_updated_ms        = now_ms;
    update_suggestion(bucket.suggested_resident_id, bucket.suggested_resident_sim);
    add_sample_to_list(bucket.samples, std::move(sample));
    prune_pending(now_ms);
}

void TripAssembler::on_car_calls_mask(std::uint16_t current_mask, bool valid) {
    if (!valid) {
        car_mask_valid_ = false;
        prev_car_mask_  = 0;
        return;
    }
    if (!car_mask_valid_) {
        prev_car_mask_  = current_mask;
        car_mask_valid_ = true;
        return;
    }
    const std::uint16_t newly_pressed =
        static_cast<std::uint16_t>(current_mask & static_cast<std::uint16_t>(~prev_car_mask_));
    prev_car_mask_ = current_mask;
    if (newly_pressed == 0) return;
    for (auto& kv : open_trips_) {
        kv.second.record.car_calls_mask |= newly_pressed;
    }
}

void TripAssembler::on_floor_call_decision(const FloorCallDecision& decision, double now_ms) {
    if (decision.request_id.empty()) {
        // Still capture immediate rejected/suppressed call decisions for resident_id without
        // request_id.
        if (decision.resident_id < 0 || decision.floor < 0) return;
        for (auto& kv : open_trips_) {
            bool matches_resident = false;
            for (const auto& c : kv.second.confirmations) {
                if (c.resident_id == decision.resident_id) {
                    matches_resident = true;
                    break;
                }
            }
            if (matches_resident && kv.second.record.call_state.empty()) {
                kv.second.record.called_floor     = decision.floor;
                kv.second.record.has_called_floor = true;
                kv.second.record.call_state       = floor_call_state_name(decision.state);
            }
        }
        return;
    }

    const std::string state_name = floor_call_state_name(decision.state);
    bool              updated    = false;
    for (auto& link : recent_auto_calls_) {
        if (link.request_id == decision.request_id) {
            link.state        = state_name;
            link.timestamp_ms = now_ms;
            if (decision.floor >= 0) link.floor = decision.floor;
            if (decision.resident_id >= 0) link.resident_id = decision.resident_id;
            updated = true;
            break;
        }
    }
    if (!updated) {
        AutoCallLink link;
        link.request_id   = decision.request_id;
        link.resident_id  = decision.resident_id;
        link.floor        = decision.floor;
        link.state        = state_name;
        link.timestamp_ms = now_ms;
        recent_auto_calls_.push_back(std::move(link));
        while (recent_auto_calls_.size() > kMaxRecentCallHistory) {
            recent_auto_calls_.pop_front();
        }
    }

    auto apply_to_trip = [&](ActiveTrip& trip) {
        bool matches_resident = (trip.record.resident_id == decision.resident_id);
        if (!matches_resident) {
            for (const auto& c : trip.confirmations) {
                if (c.resident_id == decision.resident_id) {
                    matches_resident = true;
                    break;
                }
            }
        }
        if (matches_resident && decision.floor >= 0) {
            trip.record.called_floor     = decision.floor;
            trip.record.has_called_floor = true;
            trip.record.call_state       = state_name;
        }
    };
    for (auto& kv : open_trips_)
        apply_to_trip(kv.second);
    for (auto& t : awaiting_trips_)
        apply_to_trip(t);
}

void TripAssembler::on_auto_call_confirmations(
    const std::vector<FloorCallConfirmEvent>& confirmations) {
    for (const auto& ev : confirmations) {
        for (auto& link : recent_auto_calls_) {
            if (link.request_id == ev.request_id) {
                link.register_confirmed = ev.confirmed ? 1 : 0;
                break;
            }
        }
    }
}

void TripAssembler::on_completed_calls(const std::vector<CarCallRecord>&  car_calls,
                                       const std::vector<HallCallRecord>& hall_calls,
                                       double                             now_ms) {
    for (const auto& cc : car_calls) {
        recent_car_calls_.push_back(cc);
    }
    for (const auto& hc : hall_calls) {
        recent_hall_calls_.push_back(hc);
    }
    while (recent_car_calls_.size() > kMaxRecentCallHistory &&
           now_ms - recent_car_calls_.front().cleared_at_ms > kCallHistoryRetentionMs) {
        recent_car_calls_.pop_front();
    }
    while (recent_hall_calls_.size() > kMaxRecentCallHistory &&
           now_ms - recent_hall_calls_.front().cleared_at_ms > kCallHistoryRetentionMs) {
        recent_hall_calls_.pop_front();
    }
    while (recent_car_calls_.size() > kMaxRecentCallHistory * 2) {
        recent_car_calls_.pop_front();
    }
    while (recent_hall_calls_.size() > kMaxRecentCallHistory * 2) {
        recent_hall_calls_.pop_front();
    }
}

TripAssemblerOutput TripAssembler::on_timeline_events(const std::vector<TimelineEvent>& events,
                                                      int                 current_occ_count,
                                                      OccupancyConfidence current_occ_confidence,
                                                      double              now_ms) {
    TripAssemblerOutput out;
    for (const TimelineEvent& ev : events) {
        if (ev.slave != config_.slave) continue;
        switch (ev.type) {
        case TimelineEventType::StopOpened: {
            StopBufferEntry entry;
            entry.record = ev.stop;
            stop_buffer_.push_back(std::move(entry));
            break;
        }
        case TimelineEventType::DoorOpened:
        case TimelineEventType::DoorClosed:
        case TimelineEventType::StopClosed: {
            for (auto it = stop_buffer_.rbegin(); it != stop_buffer_.rend(); ++it) {
                if (it->record.stop_key == ev.stop.stop_key) {
                    const int kept_boarded  = it->record.boarded;
                    const int kept_alighted = it->record.alighted;
                    it->record              = ev.stop;
                    it->record.boarded      = kept_boarded;
                    it->record.alighted     = kept_alighted;
                    break;
                }
            }
            break;
        }
        case TimelineEventType::RunStarted: {
            current_open_run_key_ = ev.run.run_key;
            if (!ev.run.from_stop_key.empty()) {
                for (auto it = stop_buffer_.rbegin(); it != stop_buffer_.rend(); ++it) {
                    if (it->record.stop_key == ev.run.from_stop_key) {
                        it->next_run_direction = ev.run.direction;
                        break;
                    }
                }
            }
            for (auto& kv : open_trips_) {
                kv.second.record.rides += 1;
                kv.second.last_run_timestamp_ms = ev.timestamp_ms;
                out.run_passengers.push_back(
                    RunPassengerLink{ev.run.run_key, kv.second.record.trip_key});
            }
            break;
        }
        case TimelineEventType::RunEnded: {
            CabinRunRecord run = ev.run;
            if (current_occ_count > config_.cabin_capacity) {
                run.has_load_count = false;
                run.load_count     = 0;
                run.load_confidence.clear();
                run.flags |= TrafficFlag::LOAD_IMPLAUSIBLE;
            } else {
                run.has_load_count  = true;
                run.load_count      = std::max(0, current_occ_count);
                run.load_confidence = current_occ_confidence == OccupancyConfidence::Certain
                                          ? "certain"
                                          : "uncertain";
            }
            out.completed_runs.push_back(std::move(run));
            if (current_open_run_key_ == ev.run.run_key) {
                current_open_run_key_.clear();
            }
            break;
        }
        case TimelineEventType::RegisterGap: {
            for (auto& kv : open_trips_) {
                kv.second.had_gap_while_running = true;
            }
            break;
        }
        }
    }

    resolve_waiting_trips(now_ms, false, out);
    flush_ready_stops(now_ms, false, out);
    return out;
}

TripAssembler::ActiveTrip TripAssembler::close_open_trip(std::uint64_t occ_id, double exited_at_ms,
                                                         std::uint32_t extra_flags,
                                                         bool          exit_already_resolved) {
    auto       it   = open_trips_.find(occ_id);
    ActiveTrip trip = std::move(it->second);
    open_trips_.erase(it);
    trip.record.exited_at_ms = exited_at_ms;
    trip.exit_timestamp_ms   = exited_at_ms;
    trip.exit_resolved       = exit_already_resolved;
    trip.record.flags |= extra_flags;
    return trip;
}

TripAssemblerOutput TripAssembler::on_occupancy(const OccupancySnapshot& snapshot, double now_ms) {
    TripAssemblerOutput out;

    for (const OccupancyEvent& ev : snapshot.events) {
        if (ev.direction == OccupancyEventDirection::Entry) {
            if (ev.occupant_id == 0) continue;

            if (open_trips_.find(ev.occupant_id) != open_trips_.end()) {
                ActiveTrip prev =
                    close_open_trip(ev.occupant_id, -1.0, TrafficFlag::EXIT_UNOBSERVED, true);
                awaiting_trips_.push_back(std::move(prev));
            }
            while (open_trips_.size() >= kMaxOpenTrips) {
                auto oldest = open_trips_.begin();
                for (auto it = open_trips_.begin(); it != open_trips_.end(); ++it) {
                    if (it->second.record.entered_at_ms < oldest->second.record.entered_at_ms) {
                        oldest = it;
                    }
                }
                ActiveTrip evicted =
                    close_open_trip(oldest->first, -1.0, TrafficFlag::EXIT_UNOBSERVED, true);
                awaiting_trips_.push_back(std::move(evicted));
            }

            ActiveTrip trip;
            trip.record.trip_key      = next_trip_key();
            trip.record.cabin_id      = config_.cabin_id;
            trip.record.entered_at_ms = ev.timestamp_ms;
            trip.entry_timestamp_ms   = ev.timestamp_ms;
            trip.active_since_ms      = ev.timestamp_ms;
            if (snapshot.confidence == OccupancyConfidence::Uncertain) {
                trip.record.flags |= TrafficFlag::OCCUPANCY_UNCERTAIN_IN;
            }

            const auto pit = pending_by_track_.find(ev.track_id);
            if (pit != pending_by_track_.end()) {
                for (const auto& c : pit->second.confirmations) {
                    if (ev.timestamp_ms - c.timestamp_ms <= config_.pending_identity_ttl_ms) {
                        trip.confirmations.push_back(c);
                    }
                }
                for (const auto& s : pit->second.samples) {
                    if (ev.timestamp_ms - s.timestamp_ms <= config_.pending_identity_ttl_ms) {
                        add_sample_to_list(trip.record.face_samples, s);
                    }
                }
                if (pit->second.suggested_resident_id >= 0 &&
                    ev.timestamp_ms - pit->second.last_updated_ms <=
                        config_.pending_identity_ttl_ms) {
                    trip.record.suggested_resident_id  = pit->second.suggested_resident_id;
                    trip.record.suggested_resident_sim = pit->second.suggested_resident_sim;
                }
                pending_by_track_.erase(pit);
            }

            std::uint32_t entry_flags = 0;
            if (try_resolve_endpoint(trip.entry_timestamp_ms, true, now_ms, false,
                                     trip.record.floor_in, trip.record.has_floor_in,
                                     trip.record.boarded_stop_key, entry_flags)) {
                trip.entry_resolved = true;
                trip.record.flags |= entry_flags;
            }
            open_trips_.emplace(ev.occupant_id, std::move(trip));
        } else {
            // Exit
            const std::uint32_t unc_out = snapshot.confidence == OccupancyConfidence::Uncertain
                                              ? TrafficFlag::OCCUPANCY_UNCERTAIN_OUT
                                              : 0u;
            auto oit = (ev.occupant_id != 0) ? open_trips_.find(ev.occupant_id) : open_trips_.end();
            if (oit != open_trips_.end()) {
                ActiveTrip closing =
                    close_open_trip(ev.occupant_id, ev.timestamp_ms, unc_out, false);
                std::uint32_t exit_flags = 0;
                if (try_resolve_endpoint(closing.exit_timestamp_ms, false, now_ms, false,
                                         closing.record.floor_out, closing.record.has_floor_out,
                                         closing.record.alighted_stop_key, exit_flags)) {
                    closing.exit_resolved = true;
                    closing.record.flags |= exit_flags;
                }
                awaiting_trips_.push_back(std::move(closing));
            } else {
                ActiveTrip unmatched;
                unmatched.record.trip_key     = next_trip_key();
                unmatched.record.cabin_id     = config_.cabin_id;
                unmatched.entry_resolved      = true;
                unmatched.exit_timestamp_ms   = ev.timestamp_ms;
                unmatched.record.exited_at_ms = ev.timestamp_ms;
                unmatched.active_since_ms     = ev.timestamp_ms;
                unmatched.record.flags |= TrafficFlag::ENTRY_UNOBSERVED | unc_out;
                std::uint32_t exit_flags = 0;
                if (try_resolve_endpoint(unmatched.exit_timestamp_ms, false, now_ms, false,
                                         unmatched.record.floor_out, unmatched.record.has_floor_out,
                                         unmatched.record.alighted_stop_key, exit_flags)) {
                    unmatched.exit_resolved = true;
                    unmatched.record.flags |= exit_flags;
                }
                awaiting_trips_.push_back(std::move(unmatched));
            }
        }
    }

    track_to_occupant_.clear();
    for (const auto& pair : snapshot.track_occupants) {
        if (pair.second != 0) {
            track_to_occupant_[pair.first] = pair.second;
        }
    }

    // Reconcile empty: any open trips remaining when occupancy count is 0 are
    // closed as exit_unobserved.
    if (snapshot.count == 0 && !open_trips_.empty()) {
        std::vector<std::uint64_t> ids;
        ids.reserve(open_trips_.size());
        for (const auto& kv : open_trips_)
            ids.push_back(kv.first);
        for (std::uint64_t id : ids) {
            ActiveTrip closing = close_open_trip(id, -1.0, TrafficFlag::EXIT_UNOBSERVED, true);
            awaiting_trips_.push_back(std::move(closing));
        }
    }

    resolve_waiting_trips(now_ms, false, out);
    flush_ready_stops(now_ms, false, out);
    return out;
}

bool TripAssembler::try_resolve_endpoint(double event_ms, bool is_entry, double now_ms,
                                         bool force_flush, int& out_floor, bool& out_has_floor,
                                         std::string& out_stop_key, std::uint32_t& out_flags) {
    const double tol = config_.floor_tolerance_ms;

    // 1. Strict containment [arrived_at, departed_at] wins immediately unless at an exact edge.
    bool        found_strict = false;
    std::size_t strict_idx   = 0;
    for (std::size_t i = 0; i < stop_buffer_.size(); ++i) {
        const CabinStopRecord& s = stop_buffer_[i].record;
        if (distance_to_stop(s, event_ms) == 0.0) {
            if (!found_strict) {
                found_strict = true;
                strict_idx   = i;
            } else {
                const auto& cur  = stop_buffer_[strict_idx].record;
                const bool  pick = is_entry ? (s.arrived_at_ms < cur.arrived_at_ms)
                                            : (s.arrived_at_ms > cur.arrived_at_ms);
                if (pick) strict_idx = i;
            }
        }
    }
    if (found_strict) {
        StopBufferEntry& chosen = stop_buffer_[strict_idx];
        out_stop_key            = chosen.record.stop_key;
        if (is_entry)
            chosen.record.boarded += 1;
        else
            chosen.record.alighted += 1;
        if (chosen.record.has_floor) {
            out_has_floor = true;
            out_floor     = chosen.record.floor;
        } else {
            out_has_floor = false;
            out_floor     = -1;
            out_flags |= TrafficFlag::FLOOR_UNKNOWN;
            if ((chosen.record.flags & TrafficFlag::REGISTER_GAP) != 0) {
                out_flags |= TrafficFlag::REGISTER_GAP;
            }
        }
        return true;
    }

    // 2. Not strictly inside any stop yet: wait until either tol elapsed or a future stop arrives
    //    at or after event_ms (after which no future stop can be closer).
    bool stop_after_event = false;
    for (const auto& entry : stop_buffer_) {
        if (entry.record.arrived_at_ms >= event_ms) {
            stop_after_event = true;
            break;
        }
    }
    if (!force_flush && !stop_after_event && (now_ms - event_ms <= tol)) {
        return false;
    }

    // 3. Decide best stop in tolerance window [arrived_at - tol, departed_at + tol].
    bool        found_cand = false;
    std::size_t best_idx   = 0;
    double      best_dist  = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < stop_buffer_.size(); ++i) {
        const CabinStopRecord& s = stop_buffer_[i].record;
        if (!stop_contains_range(s, event_ms, tol)) continue;
        const double dist = distance_to_stop(s, event_ms);
        if (!found_cand || dist < best_dist - 1e-6) {
            found_cand = true;
            best_idx   = i;
            best_dist  = dist;
        } else if (std::fabs(dist - best_dist) <= 1e-6) {
            const auto& cur  = stop_buffer_[best_idx].record;
            const bool  pick = is_entry ? (s.arrived_at_ms < cur.arrived_at_ms)
                                        : (s.arrived_at_ms > cur.arrived_at_ms);
            if (pick) {
                best_idx  = i;
                best_dist = dist;
            }
        }
    }

    if (!found_cand) {
        out_has_floor = false;
        out_floor     = -1;
        out_stop_key.clear();
        out_flags |= TrafficFlag::FLOOR_UNKNOWN;
        return true;
    }

    StopBufferEntry& chosen = stop_buffer_[best_idx];
    out_stop_key            = chosen.record.stop_key;
    if (is_entry)
        chosen.record.boarded += 1;
    else
        chosen.record.alighted += 1;
    if (chosen.record.has_floor) {
        out_has_floor = true;
        out_floor     = chosen.record.floor;
    } else {
        out_has_floor = false;
        out_floor     = -1;
        out_flags |= TrafficFlag::FLOOR_UNKNOWN;
        if ((chosen.record.flags & TrafficFlag::REGISTER_GAP) != 0) {
            out_flags |= TrafficFlag::REGISTER_GAP;
        }
    }
    return true;
}

void TripAssembler::apply_auto_call_links(ActiveTrip& trip) const {
    if (trip.record.resident_id < 0 || !trip.record.call_state.empty()) {
        // Also populate register confirmation if we have matching request_id.
    }
    if (trip.record.resident_id < 0) return;

    for (auto it = recent_auto_calls_.rbegin(); it != recent_auto_calls_.rend(); ++it) {
        if (it->resident_id != trip.record.resident_id) continue;
        if (trip.record.entered_at_ms >= 0.0 &&
            it->timestamp_ms < trip.record.entered_at_ms - config_.pending_identity_ttl_ms) {
            continue;
        }
        if (trip.record.exited_at_ms >= 0.0 && it->timestamp_ms > trip.record.exited_at_ms) {
            continue;
        }
        if (it->floor >= 0) {
            trip.record.called_floor     = it->floor;
            trip.record.has_called_floor = true;
        }
        trip.record.call_state              = it->state;
        trip.record.call_register_confirmed = it->register_confirmed;
        return;
    }
}

void TripAssembler::finalize_and_emit(ActiveTrip& trip, TripAssemblerOutput& out) {
    PassengerTripRecord& rec = trip.record;

    // 1. Identity resolution
    if (!trip.confirmations.empty()) {
        std::set<std::int64_t> distinct_residents;
        ResidentConfirmation   best = trip.confirmations.front();
        for (const auto& c : trip.confirmations) {
            distinct_residents.insert(c.resident_id);
            if (c.similarity > best.similarity) {
                best = c;
            }
        }
        rec.identity_kind          = "resident";
        rec.resident_id            = best.resident_id;
        rec.identity_sim           = best.similarity;
        rec.suggested_resident_id  = -1;
        rec.suggested_resident_sim = -1.0f;
        if (distinct_residents.size() > 1) {
            rec.flags |= TrafficFlag::IDENTITY_CONFLICT;
        }
        if (config_.save_faces == TrafficSaveFaces::Visitors) {
            for (auto& s : rec.face_samples) {
                s.crop_bgr_112.clear();
                s.crop_w = 0;
                s.crop_h = 0;
            }
        }
        apply_auto_call_links(trip);
    } else if (config_.visitor_reid && rec.face_samples.size() >= 2) {
        rec.identity_kind = "visitor"; // VisitorGallery in TrafficLogWriter resolves/downgrades
    } else {
        rec.identity_kind = "unidentified";
    }

    // 2. no_ride check (same floor observed in & out without a cabin ride)
    if ((rec.flags & (TrafficFlag::ENTRY_UNOBSERVED | TrafficFlag::EXIT_UNOBSERVED)) == 0 &&
        rec.has_floor_in && rec.has_floor_out && rec.floor_in == rec.floor_out && rec.rides == 0) {
        rec.flags |= TrafficFlag::NO_RIDE;
    }

    // 3. Link served hall call at boarded_stop_key (R13.4)
    if (!rec.boarded_stop_key.empty()) {
        std::string expected_dir;
        for (const auto& sb : stop_buffer_) {
            if (sb.record.stop_key == rec.boarded_stop_key) {
                expected_dir = sb.next_run_direction;
                break;
            }
        }
        std::string fallback_key;
        for (auto it = recent_hall_calls_.rbegin(); it != recent_hall_calls_.rend(); ++it) {
            if (it->outcome == "served" && it->served_stop_key == rec.boarded_stop_key) {
                if (fallback_key.empty()) fallback_key = it->call_key;
                if (!expected_dir.empty() && it->direction == expected_dir) {
                    rec.hall_call_key = it->call_key;
                    break;
                }
            }
        }
        if (rec.hall_call_key.empty()) rec.hall_call_key = std::move(fallback_key);
    }

    // 4. Link served car call at alighted_stop_key (R14.4)
    if (!rec.alighted_stop_key.empty()) {
        for (auto it = recent_car_calls_.rbegin(); it != recent_car_calls_.rend(); ++it) {
            if (it->outcome == "served" && it->served_stop_key == rec.alighted_stop_key) {
                rec.car_call_key = it->call_key;
                break;
            }
        }
    }

    // 5. Confidence
    rec.confidence = compute_confidence(rec.flags);
    out.completed_trips.push_back(std::move(rec));
}

void TripAssembler::resolve_waiting_trips(double now_ms, bool force_flush,
                                          TripAssemblerOutput& out) {
    // Also try resolving entries on still-open trips if unresolved.
    for (auto& kv : open_trips_) {
        ActiveTrip& trip = kv.second;
        if (!trip.entry_resolved) {
            std::uint32_t entry_flags = 0;
            if (try_resolve_endpoint(trip.entry_timestamp_ms, true, now_ms, force_flush,
                                     trip.record.floor_in, trip.record.has_floor_in,
                                     trip.record.boarded_stop_key, entry_flags)) {
                trip.entry_resolved = true;
                trip.record.flags |= entry_flags;
            }
        }
    }

    for (auto it = awaiting_trips_.begin(); it != awaiting_trips_.end();) {
        if (!it->entry_resolved) {
            std::uint32_t entry_flags = 0;
            if (try_resolve_endpoint(it->entry_timestamp_ms, true, now_ms, force_flush,
                                     it->record.floor_in, it->record.has_floor_in,
                                     it->record.boarded_stop_key, entry_flags)) {
                it->entry_resolved = true;
                it->record.flags |= entry_flags;
            }
        }
        if (!it->exit_resolved) {
            std::uint32_t exit_flags = 0;
            if (try_resolve_endpoint(it->exit_timestamp_ms, false, now_ms, force_flush,
                                     it->record.floor_out, it->record.has_floor_out,
                                     it->record.alighted_stop_key, exit_flags)) {
                it->exit_resolved = true;
                it->record.flags |= exit_flags;
            }
        }
        if (it->entry_resolved && it->exit_resolved) {
            finalize_and_emit(*it, out);
            it = awaiting_trips_.erase(it);
        } else {
            ++it;
        }
    }
}

void TripAssembler::flush_ready_stops(double now_ms, bool force_flush, TripAssemblerOutput& out) {
    const double tol = config_.floor_tolerance_ms;
    while (!stop_buffer_.empty()) {
        const CabinStopRecord& front = stop_buffer_.front().record;
        if (!force_flush) {
            if (!front.closed) break;
            if (now_ms - front.departed_at_ms <= tol) break;
            bool referenced_by_pending = false;
            for (const auto& kv : open_trips_) {
                if (!kv.second.entry_resolved &&
                    kv.second.entry_timestamp_ms <= front.departed_at_ms + tol) {
                    referenced_by_pending = true;
                    break;
                }
            }
            if (!referenced_by_pending) {
                for (const auto& t : awaiting_trips_) {
                    if ((!t.entry_resolved && t.entry_timestamp_ms <= front.departed_at_ms + tol) ||
                        (!t.exit_resolved && t.exit_timestamp_ms <= front.departed_at_ms + tol)) {
                        referenced_by_pending = true;
                        break;
                    }
                }
            }
            if (referenced_by_pending) break;
        }
        out.completed_stops.push_back(std::move(stop_buffer_.front().record));
        stop_buffer_.pop_front();
    }
}

TripAssemblerOutput TripAssembler::poll(double now_ms) {
    TripAssemblerOutput out;
    prune_pending(now_ms);

    // Expire open trips exceeding max_trip_ms with exit_unobserved.
    std::vector<std::uint64_t> timed_out;
    for (const auto& kv : open_trips_) {
        if (now_ms - kv.second.record.entered_at_ms > config_.max_trip_ms) {
            timed_out.push_back(kv.first);
        }
    }
    for (std::uint64_t id : timed_out) {
        ActiveTrip closing = close_open_trip(id, -1.0, TrafficFlag::EXIT_UNOBSERVED, true);
        awaiting_trips_.push_back(std::move(closing));
    }

    resolve_waiting_trips(now_ms, false, out);
    flush_ready_stops(now_ms, false, out);
    return out;
}

TripAssemblerOutput TripAssembler::shutdown(double now_ms) {
    TripAssemblerOutput        out;
    std::vector<std::uint64_t> remaining;
    remaining.reserve(open_trips_.size());
    for (const auto& kv : open_trips_)
        remaining.push_back(kv.first);

    for (std::uint64_t id : remaining) {
        ActiveTrip closing =
            close_open_trip(id, -1.0, TrafficFlag::EXIT_UNOBSERVED | TrafficFlag::SHUTDOWN, true);
        awaiting_trips_.push_back(std::move(closing));
    }
    for (auto& t : awaiting_trips_) {
        t.record.flags |= TrafficFlag::SHUTDOWN;
    }

    resolve_waiting_trips(now_ms, true, out);
    flush_ready_stops(now_ms, true, out);
    return out;
}
