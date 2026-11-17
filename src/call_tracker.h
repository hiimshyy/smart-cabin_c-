#pragma once

#include "elevator_registers.h"
#include "elevator_state.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct CarCallRecord {
    std::string   call_key;
    int           cabin_id         = 2;
    int           floor            = -1;
    double        registered_at_ms = -1.0;
    std::string   registered_stop_key;
    int           origin_floor     = -1;
    bool          has_origin_floor = false;
    double        cleared_at_ms    = -1.0;
    std::string   served_stop_key;
    std::string   outcome;
    std::string   source = "button";
    std::uint32_t flags  = 0;
};

struct HallCallRecord {
    std::string   call_key;
    int           floor = -1;
    std::string   direction;
    double        registered_at_ms = -1.0;
    double        cleared_at_ms    = -1.0;
    int           served_cabin_id  = 0;
    bool          has_served_cabin = false;
    std::string   served_stop_key;
    double        door_opened_at_ms = -1.0;
    std::int64_t  wait_ms           = -1;
    std::string   outcome;
    std::uint32_t flags = 0;
};

struct FloorCallConfirmEvent {
    std::string request_id;
    int         floor     = 0;
    bool        confirmed = false;
};

struct CallTrackerConfig {
    int         cabin_id           = 2;
    int         primary_slave      = 2;
    int         secondary_slave    = 1;
    int         floor_min          = 0;
    int         floor_max          = 9;
    double      floor_tolerance_ms = 10000.0;
    double      call_confirm_ms    = 15000.0;
    std::string session_hex        = "0000000000000001";
};

struct CallStepResult {
    std::vector<CarCallRecord>         completed_car_calls;
    std::vector<HallCallRecord>        completed_hall_calls;
    std::vector<FloorCallConfirmEvent> auto_confirmations;
};

class CallTracker {
public:
    explicit CallTracker(CallTrackerConfig config = {});

    void on_auto_call_sent(const std::string& request_id, int floor, double sent_at_ms);

    CallStepResult on_step(const SlaveRegisterState& primary_state, TimelineState primary_tl_state,
                           const std::deque<CabinStopRecord>& primary_stops,
                           const std::deque<CabinStopRecord>& secondary_stops, bool gap_triggered,
                           double now_ms);

    CallStepResult poll(const std::deque<CabinStopRecord>& primary_stops,
                        const std::deque<CabinStopRecord>& secondary_stops, double now_ms);

    CallStepResult shutdown(const std::deque<CabinStopRecord>& primary_stops,
                            const std::deque<CabinStopRecord>& secondary_stops, double now_ms);

    std::size_t   open_call_count() const;
    std::uint64_t invalid_mask_bits_count() const {
        return invalid_mask_bits_count_;
    }

private:
    struct PendingAutoCall {
        std::string request_id;
        int         floor      = 0;
        double      sent_at_ms = 0.0;
    };

    void        resolve_pending(const std::deque<CabinStopRecord>& primary_stops,
                                const std::deque<CabinStopRecord>& secondary_stops, double now_ms,
                                bool force_flush, const char* forced_outcome, std::uint32_t forced_flag,
                                CallStepResult& out);
    void        expire_auto_calls(double now_ms, CallStepResult& out);
    std::string next_call_key(const char* kind);

    CallTrackerConfig             config_;
    bool                          mask_initialized_ = false;
    std::uint16_t                 prev_car_mask_    = 0;
    std::uint16_t                 prev_hall_up_     = 0;
    std::uint16_t                 prev_hall_down_   = 0;
    std::map<int, CarCallRecord>  open_car_;
    std::map<int, HallCallRecord> open_hall_up_;
    std::map<int, HallCallRecord> open_hall_down_;
    std::vector<CarCallRecord>    awaiting_car_;
    std::vector<HallCallRecord>   awaiting_hall_;
    std::vector<PendingAutoCall>  pending_auto_;
    std::uint64_t                 next_seq_                = 1;
    std::uint64_t                 invalid_mask_bits_count_ = 0;
};
