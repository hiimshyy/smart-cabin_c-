#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

// Periodically samples host/process health without touching the realtime
// inference loop. Records are emitted through the application's normal logger.
class SystemMetrics {
public:
    explicit SystemMetrics(std::string storage_path = "/home/orangepi/face_recog_app",
                           int interval_ms = 30000);
    ~SystemMetrics();

    SystemMetrics(const SystemMetrics&) = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;

    void start();
    void stop();

private:
    void worker_loop();

    std::string storage_path_;
    int interval_ms_;
    std::atomic<bool> stop_requested_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};
