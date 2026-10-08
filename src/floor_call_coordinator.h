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
    int cabin_id = 1;
    int floors_min = 1;
    int floors_max = 30;
    int session_empty_ms = 5000;
    int max_commands_per_visit = 16;
    std::string request_id_prefix = "ai-core";
};

struct FloorCallCommand {
    std::uint64_t visit_id = 0;
    std::uint64_t sequence = 0;
    std::string request_id;
    std::int64_t resident_id = -1;
    int subject_key = -1;
    int floor = 0;
    std::int64_t value = 0;
    float similarity = -1.0f;
    double created_at_ms = 0.0;
};

struct FloorCallDecision {
    std::uint64_t visit_id = 0;
    std::string request_id;
    std::int64_t resident_id = -1;
    int floor = 0;
    FloorCallState state = FloorCallState::Rejected;
    std::string reason;
};

// FloorCallCoordinator owns the policy for a single cabin visit.  Its enqueue
// callback must only place work onto a non-blocking transport queue.
class FloorCallCoordinator {
public:
    using EnqueueFn = std::function<bool(const FloorCallCommand&)>;
    using DecisionFn = std::function<void(const FloorCallDecision&)>;

    explicit FloorCallCoordinator(FloorCallConfig config = {}, EnqueueFn enqueue = {});

    void set_config(const FloorCallConfig& config);
    void set_enqueue(EnqueueFn enqueue);
    void set_decision_callback(DecisionFn callback);

    // active_subject_keys comes from the person tracker: both Tracked and
    // Lost tracks that are still inside the tracker timeout count as present.
    void on_presence(const std::vector<int>& active_subject_keys, double now_ms);

    // Called once per confirmed identity.  A resident is processed at most
    // once per visit and a floor is queued at most once per visit.
    FloorCallDecision on_confirmed(int subject_key, std::int64_t resident_id,
                                   int home_floor, float similarity, double now_ms);

    // Called asynchronously by SocketIoClient.  It is safe to invoke from its
    // worker thread; callbacks are dispatched after the coordinator lock.
    void on_transport_state(const std::string& request_id, FloorCallState state,
                            const std::string& reason = "");

    std::uint64_t active_visit_id() const;
    size_t command_count() const;

private:
    struct CommandRecord {
        FloorCallCommand command;
        FloorCallState state = FloorCallState::Queued;
    };

    void start_visit_locked(double now_ms);
    void finish_visit_locked(double now_ms);
    FloorCallDecision make_decision_locked(std::int64_t resident_id, int floor,
                                           FloorCallState state,
                                           const std::string& reason,
                                           const std::string& request_id = "") const;
    static bool terminal(FloorCallState state);
    static bool encode_floor(int floor, std::int64_t& value);
    void dispatch(FloorCallDecision decision, DecisionFn callback) const;

    mutable std::mutex mutex_;
    FloorCallConfig config_;
    EnqueueFn enqueue_;
    DecisionFn decision_callback_;
    std::uint64_t next_visit_id_ = 1;
    std::uint64_t visit_id_ = 0;
    std::uint64_t next_sequence_ = 1;
    double empty_since_ms_ = -1.0;
    std::map<std::int64_t, FloorCallDecision> resident_decisions_;
    std::map<int, std::string> floor_request_ids_;
    std::map<std::string, CommandRecord> commands_;
};
