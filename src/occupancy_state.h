#pragma once

#include "atomic_save_test.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

enum class OccupancyConfidence : uint8_t { Uncertain, Certain };
enum class OccupancyLoadStatus : uint8_t { Loaded, Missing, Invalid };

void set_occupancy_state_save_failure_for_test(AtomicSaveFailurePoint point);

struct OccupancyState {
    int                 count       = 0;
    OccupancyConfidence confidence  = OccupancyConfidence::Uncertain;
    int64_t             generation  = 0;
    double              saved_at_ms = -1.0;
};

// Owns occupancy state and persists requested snapshots on a bounded worker.
class OccupancyStateHolder {
public:
    explicit OccupancyStateHolder(std::string path);
    ~OccupancyStateHolder();

    OccupancyStateHolder(const OccupancyStateHolder&)            = delete;
    OccupancyStateHolder& operator=(const OccupancyStateHolder&) = delete;

    // Missing or invalid files reset to 0/Uncertain, return false, and warn.
    bool load(std::string& error, OccupancyLoadStatus* status = nullptr);

    // Synchronous save for setup/tools/tests only. The frame loop uses request_save().
    bool save(std::string& error);
    void request_save(int count, OccupancyConfidence confidence);
    void close();

    void set(int count, OccupancyConfidence confidence);
    void mark_uncertain(const char* reason);

    OccupancyState     get() const;
    const std::string& path() const {
        return path_;
    }

private:
    bool save_snapshot(const OccupancyState& state, std::string& error) const;
    void worker_loop();
    void reset_state();

    std::string                   path_;
    mutable std::mutex            mutex_;
    std::condition_variable       cv_;
    OccupancyState                state_;
    std::optional<OccupancyState> pending_;
    bool                          stopping_ = false;
    bool                          closed_   = false;
    std::thread                   worker_;
};
