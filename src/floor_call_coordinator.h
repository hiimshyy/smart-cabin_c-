#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Business state for automatic cabin floor calls.  This module deliberately
// has no dependency on networking, database, camera or tracker internals so
// its safety rules can be unit tested in isolation.
enum class FloorCallState : std::uint8_t {
    Queued,
    Sent,
    AckedSuccess,
    AckedDuplicate,
    AckedError,
    Expired,
    Ambiguous,
    Suppressed,
    Rejected,
};

const char* floor_call_state_name(FloorCallState state);

struct FloorCallConfig {
    int         cabin_id               = 1;
    int         floors_min             = 0;
    int         floors_max             = 30;
    int         session_empty_ms       = 5000;
    int         max_commands_per_visit = 16;
    std::string request_id_prefix      = "ai-core";
};

enum class CabinTravelDirection : std::uint8_t {
    Unknown,
    Up,
    Down,
};

struct CabinFloorContext {
    bool                 has_current_floor = false;
    int                  current_floor     = -1;
    CabinTravelDirection direction         = CabinTravelDirection::Unknown;
};

struct FloorSelectionResult {
    int         target_floor = -1;
    std::string reason;
};

// Tracks live elevator floor/status/hall-call register signals to determine
// current floor and intended direction (preferring recent hall calls over
// potentially stale 8480 status direction bits).
class CabinFloorTracker {
public:
    static constexpr double kHallCallMemoryMs = 15000.0;

    void              reset();
    void              update(bool has_floor, int floor_index, bool status_up, bool status_down,
                             std::uint16_t hall_up_mask, bool has_hall_up, std::uint16_t hall_down_mask,
                             bool has_hall_down, double now_ms);
    CabinFloorContext snapshot(double now_ms) const;

private:
    bool                  has_current_floor_ = false;
    int                   current_floor_     = -1;
    bool                  status_up_         = false;
    bool                  status_down_       = false;
    std::uint16_t         hall_up_mask_      = 0;
    std::uint16_t         hall_down_mask_    = 0;
    std::map<int, double> last_hall_up_ms_;
    std::map<int, double> last_hall_down_ms_;
};

struct FloorCallCommand {
    std::uint64_t visit_id = 0;
    std::uint64_t sequence = 0;
    std::string   request_id;
    std::int64_t  resident_id   = -1;
    int           subject_key   = -1;
    int           floor         = -1;
    std::int64_t  value         = 0;
    float         similarity    = -1.0f;
    double        created_at_ms = 0.0;
};

struct FloorCallDecision {
    std::uint64_t  visit_id = 0;
    std::string    request_id;
    std::int64_t   resident_id = -1;
    int            floor       = -1;
    FloorCallState state       = FloorCallState::Rejected;
    std::string    reason;
};

// FloorCallCoordinator owns the policy for a single cabin visit.  Its enqueue
// callback must only place work onto a non-blocking transport queue.
class FloorCallCoordinator {
public:
    using EnqueueFn  = std::function<bool(const FloorCallCommand&)>;
    using DecisionFn = std::function<void(const FloorCallDecision&)>;

    explicit FloorCallCoordinator(FloorCallConfig config = {}, EnqueueFn enqueue = {});

    void set_config(const FloorCallConfig& config);
    void set_enqueue(EnqueueFn enqueue);
    void set_decision_callback(DecisionFn callback);

    // active_subject_keys comes from the person tracker: both Tracked and
    // Lost tracks that are still inside the tracker timeout count as present.
    void on_presence(const std::vector<int>& active_subject_keys, double now_ms);

    // Choose destination floor from home_floor / secondary_floor given live cabin context.
    static FloorSelectionResult select_target_floor(int home_floor, int secondary_floor,
                                                    const CabinFloorContext& context);

    // Called once per confirmed identity.  A resident is processed at most
    // once per visit and a floor is queued at most once per visit.
    FloorCallDecision on_confirmed(int subject_key, std::int64_t resident_id, int home_floor,
                                   float similarity, double now_ms);
    FloorCallDecision on_confirmed(int subject_key, std::int64_t resident_id, int home_floor,
                                   int secondary_floor, const CabinFloorContext& context,
                                   float similarity, double now_ms);

    // Called asynchronously by SocketIoClient.  It is safe to invoke from its
    // worker thread; callbacks are dispatched after the coordinator lock.
    void on_transport_state(const std::string& request_id, FloorCallState state,
                            const std::string& reason = "");

    std::uint64_t active_visit_id() const;
    size_t        command_count() const;

private:
    struct CommandRecord {
        FloorCallCommand command;
        FloorCallState   state = FloorCallState::Queued;
    };

    void              start_visit_locked(double now_ms);
    void              finish_visit_locked(double now_ms);
    FloorCallDecision make_decision_locked(std::int64_t resident_id, int floor,
                                           FloorCallState state, const std::string& reason,
                                           const std::string& request_id = "") const;
    static bool       terminal(FloorCallState state);
    static bool       encode_floor(int floor, std::int64_t& value);
    void              dispatch(FloorCallDecision decision, DecisionFn callback) const;

    mutable std::mutex                        mutex_;
    FloorCallConfig                           config_;
    EnqueueFn                                 enqueue_;
    DecisionFn                                decision_callback_;
    std::uint64_t                             next_visit_id_  = 1;
    std::uint64_t                             visit_id_       = 0;
    std::uint64_t                             next_sequence_  = 1;
    double                                    empty_since_ms_ = -1.0;
    std::map<std::int64_t, FloorCallDecision> resident_decisions_;
    std::map<int, std::string>                floor_request_ids_;
    std::map<std::string, CommandRecord>      commands_;
};
