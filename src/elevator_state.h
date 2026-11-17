#pragma once

#include "elevator_registers.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct RawRegisterPacket {
    std::string   message;
    std::uint64_t session     = 0;
    double        received_ms = 0.0;
};

// Bounded thread-safe inbox between SocketIoClient worker and frame loop.
// Overflow sets a sticky flag so the frame thread triggers RegisterGap.
class RegisterInbox {
public:
    explicit RegisterInbox(std::size_t capacity = 256) : capacity_(capacity) {}

    void        push(RawRegisterPacket packet);
    void        drain(std::vector<RawRegisterPacket>& out, bool& overflowed);
    std::size_t capacity() const {
        return capacity_;
    }

private:
    std::size_t                   capacity_;
    mutable std::mutex            mutex_;
    std::deque<RawRegisterPacket> queue_;
    bool                          overflow_ = false;
};

struct SlaveRegisterState {
    int                          slave           = 0;
    bool                         valid           = false;
    std::uint64_t                session         = 0;
    double                       last_message_ms = -1.0;
    std::map<int, std::uint16_t> regs;

    bool          has(int reg) const;
    std::uint16_t get(int reg, std::uint16_t fallback = 0) const;
};

struct CabinStopRecord {
    std::string   stop_key;
    int           cabin_id          = 2;
    int           slave             = 2;
    int           floor             = -1;
    bool          has_floor         = false;
    int           height_cm         = -1;
    bool          has_height        = false;
    double        arrived_at_ms     = -1.0;
    double        door_opened_at_ms = -1.0;
    double        door_closed_at_ms = -1.0;
    double        departed_at_ms    = -1.0;
    int           boarded           = 0;
    int           alighted          = 0;
    std::uint16_t raw_8471_arrive   = 0;
    bool          has_8471_arrive   = false;
    std::uint16_t raw_8472_arrive   = 0;
    bool          has_8472_arrive   = false;
    std::uint16_t raw_8480_arrive   = 0;
    bool          has_8480_arrive   = false;
    std::uint16_t raw_8471_depart   = 0;
    bool          has_8471_depart   = false;
    std::uint16_t raw_8472_depart   = 0;
    bool          has_8472_depart   = false;
    std::uint16_t raw_8480_depart   = 0;
    bool          has_8480_depart   = false;
    std::uint32_t flags             = 0;
    bool          closed            = false;
};

struct CabinRunRecord {
    std::string   run_key;
    int           cabin_id = 2;
    int           slave    = 2;
    std::string   from_stop_key;
    std::string   to_stop_key;
    int           from_floor     = -1;
    bool          has_from_floor = false;
    int           to_floor       = -1;
    bool          has_to_floor   = false;
    std::string   direction      = "unknown";
    double        departed_at_ms = -1.0;
    double        arrived_at_ms  = -1.0;
    int           load_count     = 0;
    bool          has_load_count = false;
    std::string   load_confidence;
    std::uint16_t car_calls_mask = 0;
    std::uint32_t flags          = 0;
    bool          closed         = false;
};

enum class TimelineState { Unknown, Stopped, Running };

enum class TimelineEventType {
    StopOpened,
    DoorOpened,
    DoorClosed,
    StopClosed,
    RunStarted,
    RunEnded,
    RegisterGap,
};

struct TimelineEvent {
    TimelineEventType type         = TimelineEventType::RegisterGap;
    int               slave        = 0;
    double            timestamp_ms = 0.0;
    CabinStopRecord   stop;
    CabinRunRecord    run;
};

struct CabinTimelineConfig {
    int         cabin_id          = 2;
    int         slave             = 2;
    int         floor_min         = 0;
    int         floor_max         = 9;
    std::string session_hex       = "0000000000000001";
    double      stop_retention_ms = 600000.0; // keep 10 min of stops for call matching
};

class CabinTimeline {
public:
    explicit CabinTimeline(CabinTimelineConfig config = {});

    void on_register_state(const SlaveRegisterState& state, double now_ms,
                           std::vector<TimelineEvent>& events, int& invalid_floor_count,
                           int& run_counter_mismatch_count);
    void on_register_gap(double now_ms, std::vector<TimelineEvent>& events);
    void on_shutdown(double now_ms, std::vector<TimelineEvent>& events);

    TimelineState state() const {
        return state_;
    }
    const CabinStopRecord* current_stop() const {
        return has_open_stop_ ? &open_stop_ : nullptr;
    }
    const CabinRunRecord* current_run() const {
        return has_open_run_ ? &open_run_ : nullptr;
    }
    const std::deque<CabinStopRecord>& recent_stops() const {
        return recent_stops_;
    }

private:
    void accumulate_flags(const SlaveRegisterState& state, std::uint32_t& flags) const;
    void record_stop_in_history(const CabinStopRecord& stop, double now_ms);
    void open_stop(const SlaveRegisterState& state, int floor, bool has_floor, double now_ms,
                   std::vector<TimelineEvent>& events);
    void close_stop(const SlaveRegisterState& state, double now_ms, std::uint32_t extra_flags,
                    std::vector<TimelineEvent>& events);
    void open_run(const SlaveRegisterState& state, double now_ms,
                  std::vector<TimelineEvent>& events);
    void close_run(const SlaveRegisterState& state, const std::string& to_stop_key, int to_floor,
                   bool has_to_floor, double now_ms, std::uint32_t extra_flags,
                   std::vector<TimelineEvent>& events, int& run_counter_mismatch_count);

    CabinTimelineConfig         config_;
    TimelineState               state_         = TimelineState::Unknown;
    bool                        has_open_stop_ = false;
    CabinStopRecord             open_stop_;
    bool                        has_open_run_ = false;
    CabinRunRecord              open_run_;
    std::uint64_t               next_stop_seq_        = 1;
    std::uint64_t               next_run_seq_         = 1;
    std::uint32_t               last_run_counter_     = 0;
    bool                        has_last_run_counter_ = false;
    std::deque<CabinStopRecord> recent_stops_;
};

struct ElevatorStateConfig {
    std::string device_code     = "2026010900100098";
    int         cabin_id        = 2;
    int         primary_slave   = 2;
    int         secondary_slave = 1;
    int         floor_min       = 0;
    int         floor_max       = 9;
    double      run_timeout_ms  = 60000.0;
    std::string session_hex     = "0000000000000001";
};

struct ElevatorStepResult {
    bool                       gap_triggered = false;
    std::vector<TimelineEvent> primary_events;
    std::vector<TimelineEvent> secondary_events;
};

class ElevatorStateTracker {
public:
    explicit ElevatorStateTracker(ElevatorStateConfig config = {});

    // Process one parsed CONN_DATA update. Returns timeline events for primary and secondary
    // slaves.
    ElevatorStepResult apply_update(const RegisterUpdate& update);

    // Check transport health, inbox overflow, and running-state timeout.
    ElevatorStepResult poll(double now_ms, bool socket_connected, std::uint64_t socket_session,
                            bool inbox_overflow);

    // Close open stops/runs cleanly at shutdown.
    ElevatorStepResult shutdown(double now_ms);

    const SlaveRegisterState& primary_state() const {
        return primary_state_;
    }
    const SlaveRegisterState& secondary_state() const {
        return secondary_state_;
    }
    const CabinTimeline& primary_timeline() const {
        return primary_timeline_;
    }
    const CabinTimeline& secondary_timeline() const {
        return secondary_timeline_;
    }

    std::uint64_t register_invalid_count() const {
        return register_invalid_count_;
    }
    std::uint64_t run_counter_mismatch_count() const {
        return run_counter_mismatch_count_;
    }
    double last_any_message_ms() const {
        return last_any_message_ms_;
    }

private:
    void trigger_gap(double now_ms, ElevatorStepResult& result);
    bool is_slave_complete(const SlaveRegisterState& state) const;

    ElevatorStateConfig config_;
    SlaveRegisterState  primary_state_;
    SlaveRegisterState  secondary_state_;
    CabinTimeline       primary_timeline_;
    CabinTimeline       secondary_timeline_;
    std::uint64_t       active_session_             = 0;
    bool                was_connected_              = false;
    std::uint64_t       register_invalid_count_     = 0;
    std::uint64_t       run_counter_mismatch_count_ = 0;
    double              last_any_message_ms_        = -1.0;
};
