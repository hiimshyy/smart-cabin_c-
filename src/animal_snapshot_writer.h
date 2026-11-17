#pragma once

#include "yolo_post.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AnimalSnapshotWriterConfig {
    std::string output_dir = "recordings/animals";
    std::size_t max_queue  = 32;
};

// Non-blocking background writer for animal detection snapshots.
// Bounded drop-oldest queue keeps the main inference loop off disk I/O.
class AnimalSnapshotWriter {
public:
    explicit AnimalSnapshotWriter(AnimalSnapshotWriterConfig config = {});
    ~AnimalSnapshotWriter();

    AnimalSnapshotWriter(const AnimalSnapshotWriter&)            = delete;
    AnimalSnapshotWriter& operator=(const AnimalSnapshotWriter&) = delete;

    void start();
    void close();

    void enqueue(const cv::Mat& frame_bgr, std::vector<AnimalDet> detections, int cabin_id,
                 const std::string& species, float best_score);

    bool enabled() const {
        return !config_.output_dir.empty();
    }
    std::uint64_t saved_count() const {
        return saved_count_.load();
    }
    std::uint64_t dropped_count() const {
        return dropped_count_.load();
    }

private:
    struct SnapshotJob {
        cv::Mat                frame;
        std::vector<AnimalDet> detections;
        int                    cabin_id = 1;
        std::string            species;
        float                  best_score = 0.0f;
        std::int64_t           wall_ms    = 0;
    };

    void worker_loop();

    AnimalSnapshotWriterConfig config_;
    std::mutex                 mtx_;
    std::condition_variable    cv_;
    std::deque<SnapshotJob>    queue_;
    bool                       stop_requested_ = false;
    std::thread                worker_;
    std::atomic<bool>          running_{false};
    std::atomic<std::uint64_t> saved_count_{0};
    std::atomic<std::uint64_t> dropped_count_{0};
};
