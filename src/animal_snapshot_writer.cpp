#include "animal_snapshot_writer.h"

#include "log/logger.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <utility>

namespace {

bool ensure_dir_recursive(const std::string& dir) {
    if (dir.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (cur == "/") continue;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

std::int64_t now_wall_ms() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
        .count();
}

void format_local_date_and_stamp(std::int64_t wall_ms, std::string& out_date,
                                 std::string& out_stamp) {
    const std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
    const int         ms   = static_cast<int>(wall_ms % 1000);
    std::tm           tm_buf{};
    localtime_r(&secs, &tm_buf);

    char date_buf[32];
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday);
    out_date = date_buf;

    char stamp_buf[48];
    std::snprintf(stamp_buf, sizeof(stamp_buf), "%04d%02d%02d_%02d%02d%02d_%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour,
                  tm_buf.tm_min, tm_buf.tm_sec, ms < 0 ? 0 : ms);
    out_stamp = stamp_buf;
}

} // namespace

AnimalSnapshotWriter::AnimalSnapshotWriter(AnimalSnapshotWriterConfig config)
    : config_(std::move(config)) {}

AnimalSnapshotWriter::~AnimalSnapshotWriter() {
    close();
}

void AnimalSnapshotWriter::start() {
    if (!enabled() || running_.load()) return;
    stop_requested_ = false;
    running_.store(true);
    worker_ = std::thread(&AnimalSnapshotWriter::worker_loop, this);
}

void AnimalSnapshotWriter::close() {
    if (!running_.load() && !worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void AnimalSnapshotWriter::enqueue(const cv::Mat& frame_bgr, std::vector<AnimalDet> detections,
                                   int cabin_id, const std::string& species, float best_score) {
    if (!enabled() || !running_.load() || frame_bgr.empty()) return;

    SnapshotJob job;
    job.frame      = frame_bgr.clone();
    job.detections = std::move(detections);
    job.cabin_id   = cabin_id;
    job.species    = species;
    job.best_score = best_score;
    job.wall_ms    = now_wall_ms();

    {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t           cap = std::max<std::size_t>(1, config_.max_queue);
        if (queue_.size() >= cap) {
            queue_.pop_front();
            dropped_count_.fetch_add(1);
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void AnimalSnapshotWriter::worker_loop() {
    while (true) {
        SnapshotJob job;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&] { return stop_requested_ || !queue_.empty(); });
            if (queue_.empty() && stop_requested_) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        std::string date_dir;
        std::string stamp;
        format_local_date_and_stamp(job.wall_ms, date_dir, stamp);
        const std::string sub_dir = config_.output_dir + "/" + date_dir;
        if (!ensure_dir_recursive(sub_dir)) {
            LOG_WARN("animal", "failed to prepare snapshot directory");
            continue;
        }

        for (const auto& d : job.detections) {
            const int x1 = std::max(0, static_cast<int>(d.x1));
            const int y1 = std::max(0, static_cast<int>(d.y1));
            const int x2 = std::min(job.frame.cols - 1, static_cast<int>(d.x2));
            const int y2 = std::min(job.frame.rows - 1, static_cast<int>(d.y2));
            if (x2 > x1 && y2 > y1) {
                cv::rectangle(job.frame, cv::Point(x1, y1), cv::Point(x2, y2),
                              cv::Scalar(0, 165, 255), 2);
                char label[64];
                std::snprintf(label, sizeof(label), "%s %.2f", coco_animal_name(d.class_id),
                              d.score);
                cv::putText(job.frame, label, cv::Point(x1, std::max(18, y1 - 6)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 165, 255), 2);
            }
        }

        char filename[128];
        std::snprintf(filename, sizeof(filename), "/cabin%d_%s_%s.jpg", job.cabin_id,
                      job.species.empty() ? "animal" : job.species.c_str(), stamp.c_str());
        const std::string      out_path = sub_dir + filename;
        const std::vector<int> params   = {cv::IMWRITE_JPEG_QUALITY, 90};
        if (cv::imwrite(out_path, job.frame, params)) {
            ::chmod(out_path.c_str(), 0600);
            saved_count_.fetch_add(1);
            LOG_INFO("animal", "snapshot saved species=%s score=%.2f", job.species.c_str(),
                     job.best_score);
        } else {
            LOG_WARN("animal", "failed to write snapshot species=%s", job.species.c_str());
        }
    }
}
