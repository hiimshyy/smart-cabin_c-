#include "event_video_recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "log/logger.h"

namespace fs = std::filesystem;

EventVideoRecorder::EventVideoRecorder(EventVideoRecorderConfig config)
    : config_(std::move(config)) {
    max_queued_frames_ = static_cast<std::size_t>(std::max(3.0, std::ceil(config_.fps) + 2.0));
    if (enabled()) {
        LOG_INFO("record", "enabled async dir=%s codec=MJPG fps=%.2f width=%d pre_ms=%.0f empty_ms=%.0f queue=%zu",
                 config_.output_dir.c_str(), config_.fps, config_.output_width,
                 config_.pre_roll_ms, config_.empty_stop_ms, max_queued_frames_);
        worker_ = std::thread(&EventVideoRecorder::worker_loop, this);
    }
}

EventVideoRecorder::~EventVideoRecorder() {
    close();
}

void EventVideoRecorder::buffer_frame(const cv::Mat& frame, double timestamp_ms) {
    if (!pre_roll_.empty() && pre_roll_.back().frame.size() != frame.size())
        pre_roll_.clear();
    pre_roll_.push_back({timestamp_ms, frame.clone()});

    const double oldest = timestamp_ms - config_.pre_roll_ms;
    while (!pre_roll_.empty() && pre_roll_.front().timestamp_ms < oldest)
        pre_roll_.pop_front();

    const std::size_t max_frames = static_cast<std::size_t>(
        std::max(2.0, std::ceil(config_.fps * config_.pre_roll_ms / 1000.0) + 2.0));
    while (pre_roll_.size() > max_frames) pre_roll_.pop_front();
}

void EventVideoRecorder::enqueue(WorkItem item) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (item.type == WorkType::Frame) {
            std::size_t queued_frames = 0;
            for (const WorkItem& queued : work_queue_)
                if (queued.type == WorkType::Frame) ++queued_frames;
            while (queued_frames >= max_queued_frames_) {
                auto oldest = std::find_if(work_queue_.begin(), work_queue_.end(),
                    [](const WorkItem& queued) { return queued.type == WorkType::Frame; });
                if (oldest == work_queue_.end()) break;
                work_queue_.erase(oldest);
                --queued_frames;
                ++dropped_frames_;
            }
        }
        work_queue_.push_back(std::move(item));
    }
    queue_cv_.notify_one();
}

void EventVideoRecorder::start_session(const cv::Size& frame_size, double timestamp_ms) {
    session_source_size_ = frame_size;
    const double segment_start_ms = pre_roll_.empty()
        ? timestamp_ms : pre_roll_.front().timestamp_ms;
    enqueue({WorkType::Start, segment_start_ms, frame_size, {}, {}});
    for (BufferedFrame& buffered : pre_roll_) {
        WorkItem item;
        item.type = WorkType::Frame;
        item.timestamp_ms = buffered.timestamp_ms;
        item.frame = std::move(buffered.frame);
        enqueue(std::move(item));
    }
    pre_roll_.clear();
    session_active_.store(true);
}

void EventVideoRecorder::stop_session(const char* reason, double timestamp_ms) {
    if (!session_active_.exchange(false)) return;
    WorkItem item;
    item.type = WorkType::Stop;
    item.timestamp_ms = timestamp_ms;
    item.reason = reason;
    enqueue(std::move(item));
    session_source_size_ = {};
    last_person_ms_ = -1.0;
}

bool EventVideoRecorder::open_segment(const cv::Size& source_size, double timestamp_ms) {
    if (writer_.isOpened()) finish_segment("new_session", timestamp_ms);

    std::error_code ec;
    fs::create_directories(config_.output_dir, ec);
    if (ec) {
        LOG_ERROR("record", "cannot create output dir=%s error=%s",
                  config_.output_dir.c_str(), ec.message().c_str());
        return false;
    }

    const auto wall_now = std::chrono::system_clock::now();
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_now.time_since_epoch()).count();
    const std::time_t wall_time = std::chrono::system_clock::to_time_t(wall_now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &wall_time);
#else
    localtime_r(&wall_time, &local_tm);
#endif
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d_%03lld",
                  local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                  local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
                  static_cast<long long>(wall_ms % 1000));

    const std::string stem = "cabin_" + std::to_string(config_.cabin_id) + "_" +
                             stamp + "_" + std::to_string(file_sequence_++);
    final_path_ = (fs::path(config_.output_dir) / (stem + ".avi")).string();
    partial_path_ = (fs::path(config_.output_dir) / (stem + ".partial.avi")).string();

    source_size_ = source_size;
    output_size_ = source_size;
    if (config_.output_width > 0 && source_size.width > config_.output_width) {
        const double scale = static_cast<double>(config_.output_width) / source_size.width;
        int height = std::max(2, static_cast<int>(std::lround(source_size.height * scale)));
        if (height % 2 != 0) --height;
        output_size_ = {config_.output_width, height};
    }

    writer_.open(partial_path_, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                 config_.fps, output_size_, true);
    if (!writer_.isOpened()) {
        LOG_ERROR("record", "cannot open video file=%s", partial_path_.c_str());
        partial_path_.clear();
        final_path_.clear();
        return false;
    }

    segment_start_ms_ = timestamp_ms;
    segment_last_ms_ = timestamp_ms;
    frames_written_ = 0;
    dropped_at_segment_start_ = dropped_frames_.load();
    LOG_INFO("record", "started file=%s source=%dx%d output=%dx%d",
             final_path_.c_str(), source_size.width, source_size.height,
             output_size_.width, output_size_.height);
    return true;
}

void EventVideoRecorder::write_frame(const cv::Mat& frame, double timestamp_ms) {
    if (!writer_.isOpened() || frame.empty() || frame.size() != source_size_) return;
    if (output_size_ != source_size_) {
        cv::resize(frame, resized_frame_, output_size_, 0.0, 0.0, cv::INTER_AREA);
        writer_.write(resized_frame_);
    } else {
        writer_.write(frame);
    }
    segment_last_ms_ = timestamp_ms;
    ++frames_written_;
}

void EventVideoRecorder::finish_segment(const char* reason, double timestamp_ms) {
    if (!writer_.isOpened()) return;
    writer_.release();

    std::error_code ec;
    fs::rename(partial_path_, final_path_, ec);
    if (ec) {
        LOG_ERROR("record", "finalize failed partial=%s error=%s",
                  partial_path_.c_str(), ec.message().c_str());
    } else {
        ++completed_segments_;
        const double end_ms = std::max(timestamp_ms, segment_last_ms_);
        const double duration_ms = segment_start_ms_ >= 0.0 ? end_ms - segment_start_ms_ : 0.0;
        const std::size_t dropped = dropped_frames_.load() - dropped_at_segment_start_;
        LOG_INFO("record", "stopped file=%s reason=%s frames=%zu dropped=%zu duration_ms=%.0f",
                 final_path_.c_str(), reason, frames_written_, dropped, duration_ms);
    }

    partial_path_.clear();
    final_path_.clear();
    source_size_ = {};
    output_size_ = {};
    resized_frame_.release();
    frames_written_ = 0;
    segment_start_ms_ = -1.0;
    segment_last_ms_ = -1.0;
}

void EventVideoRecorder::worker_loop() {
    while (true) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return !work_queue_.empty(); });
            item = std::move(work_queue_.front());
            work_queue_.pop_front();
        }

        if (item.type == WorkType::Start) {
            open_segment(item.frame_size, item.timestamp_ms);
        } else if (item.type == WorkType::Frame) {
            write_frame(item.frame, item.timestamp_ms);
        } else if (item.type == WorkType::Stop) {
            finish_segment(item.reason.c_str(), item.timestamp_ms);
        } else {
            finish_segment("shutdown", item.timestamp_ms);
            return;
        }
    }
}

void EventVideoRecorder::push_frame(const cv::Mat& raw_frame, double timestamp_ms) {
    if (!enabled() || raw_frame.empty() || !std::isfinite(timestamp_ms)) return;
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (closed_) return;

    if (last_timestamp_ms_ >= 0.0 && timestamp_ms < last_timestamp_ms_) {
        stop_session("timestamp_reset", last_timestamp_ms_);
        pre_roll_.clear();
    }
    last_timestamp_ms_ = timestamp_ms;

    if (recording() && raw_frame.size() != session_source_size_)
        stop_session("resolution_change", timestamp_ms);

    if (!recording()) {
        buffer_frame(raw_frame, timestamp_ms);
        if (!person_present_) return;
        start_session(raw_frame.size(), timestamp_ms);
        return;
    }

    WorkItem item;
    item.type = WorkType::Frame;
    item.timestamp_ms = timestamp_ms;
    item.frame = raw_frame.clone();
    enqueue(std::move(item));

    if (!person_present_ && last_person_ms_ >= 0.0 &&
        timestamp_ms - last_person_ms_ >= config_.empty_stop_ms) {
        stop_session("empty_timeout", timestamp_ms);
    }
}

void EventVideoRecorder::set_person_present(bool person_present, double timestamp_ms) {
    if (!enabled() || !std::isfinite(timestamp_ms)) return;
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (closed_) return;
    person_present_ = person_present;
    if (!person_present) return;

    last_person_ms_ = timestamp_ms;
    const double oldest = timestamp_ms - config_.pre_roll_ms;
    while (!pre_roll_.empty() && pre_roll_.front().timestamp_ms < oldest)
        pre_roll_.pop_front();
    if (!recording() && !pre_roll_.empty())
        start_session(pre_roll_.back().frame.size(), timestamp_ms);
}

void EventVideoRecorder::close() {
    if (!enabled()) return;

    // Do not call stop_session() while holding state_mutex_. stop_session()
    // takes the same mutex after enqueueing the final Stop item; doing so
    // deadlocks when shutdown happens during an active recording session.
    double timestamp_ms = -1.0;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (closed_) return;
        closed_ = true;
        timestamp_ms = last_timestamp_ms_;
    }

    stop_session("shutdown", timestamp_ms);
    WorkItem shutdown;
    shutdown.type = WorkType::Shutdown;
    shutdown.timestamp_ms = timestamp_ms;
    enqueue(std::move(shutdown));
    if (worker_.joinable()) worker_.join();
    pre_roll_.clear();
}
