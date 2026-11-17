#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct SystemOccupancySnapshot {
    bool enabled   = false;
    int  count     = 0;
    bool uncertain = false;
};

struct SystemTrafficSnapshot {
    bool          enabled              = false;
    std::size_t   queue_size           = 0;
    std::uint64_t dropped              = 0;
    std::size_t   open_trips           = 0;
    std::size_t   open_calls           = 0;
    std::int64_t  register_age_ms      = -1;
    std::uint64_t register_invalid     = 0;
    std::uint64_t run_counter_mismatch = 0;
    std::uint64_t visitors_active      = 0;
    std::uint64_t write_errors         = 0;
};

// Periodically samples host/process health without touching the realtime
// inference loop. Records are emitted through the application's normal logger.
class SystemMetrics {
public:
    explicit SystemMetrics(std::string storage_path = "/home/orangepi/face_recog_app",
                           int         interval_ms  = 30000);
    ~SystemMetrics();

    SystemMetrics(const SystemMetrics&)            = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;

    void start();
    void stop();

    void                    set_occupancy(int count, bool uncertain);
    SystemOccupancySnapshot occupancy_snapshot() const;

    void                  set_traffic(const SystemTrafficSnapshot& snapshot);
    SystemTrafficSnapshot traffic_snapshot() const;

private:
    void worker_loop();

    std::string             storage_path_;
    int                     interval_ms_;
    std::atomic<bool>       stop_requested_{false};
    std::atomic<uint64_t>   occupancy_snapshot_{0};
    mutable std::mutex      traffic_mtx_;
    SystemTrafficSnapshot   traffic_snapshot_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             worker_;
};
