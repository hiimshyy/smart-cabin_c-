#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

struct EventVideoRecorderConfig {
    std::string output_dir;
    double fps = 15.0;
    double pre_roll_ms = 1000.0;
    double empty_stop_ms = 3000.0;
    int output_width = 960;  // 0 keeps source resolution
    int cabin_id = 1;
};

// Records raw BGR frames in person-presence sessions. push_frame() is called
// by the capture thread so recording does not inherit inference-frame drops.
// The inference thread only updates the stable tracker presence signal. A
// worker owns VideoWriter, resize, file finalization and slow disk I/O.
class EventVideoRecorder {
public:
    explicit EventVideoRecorder(EventVideoRecorderConfig config);
    ~EventVideoRecorder();

    EventVideoRecorder(const EventVideoRecorder&) = delete;
    EventVideoRecorder& operator=(const EventVideoRecorder&) = delete;

    void push_frame(const cv::Mat& raw_frame, double timestamp_ms);
    void set_person_present(bool person_present, double timestamp_ms);
    void close();

    bool enabled() const { return !config_.output_dir.empty(); }
    bool recording() const { return session_active_.load(); }
    std::size_t completed_segments() const { return completed_segments_.load(); }
    std::size_t dropped_frames() const { return dropped_frames_.load(); }

private:
    struct BufferedFrame {
        double timestamp_ms = 0.0;
        cv::Mat frame;
    };

    enum class WorkType { Start, Frame, Stop, Shutdown };
    struct WorkItem {
        WorkType type = WorkType::Frame;
        double timestamp_ms = 0.0;
        cv::Size frame_size;
        cv::Mat frame;
        std::string reason;
    };

    void buffer_frame(const cv::Mat& frame, double timestamp_ms);
    void start_session(const cv::Size& frame_size, double timestamp_ms);
    void stop_session(const char* reason, double timestamp_ms);
    void enqueue(WorkItem item);
    void worker_loop();

    bool open_segment(const cv::Size& source_size, double timestamp_ms);
    void write_frame(const cv::Mat& frame, double timestamp_ms);
    void finish_segment(const char* reason, double timestamp_ms);

    EventVideoRecorderConfig config_;

    // Main/inference thread state.
    std::deque<BufferedFrame> pre_roll_;
    std::mutex state_mutex_;
    double last_timestamp_ms_ = -1.0;
    double last_person_ms_ = -1.0;
    cv::Size session_source_size_;
    bool person_present_ = false;
    std::atomic<bool> session_active_{false};
    bool closed_ = false;

    // Bounded handoff to the encoder worker.
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<WorkItem> work_queue_;
    std::size_t max_queued_frames_ = 17;
    std::thread worker_;

    // Worker-only encoder/file state.
    cv::VideoWriter writer_;
    cv::Size source_size_;
    cv::Size output_size_;
    cv::Mat resized_frame_;
    std::string partial_path_;
    std::string final_path_;
    double segment_start_ms_ = -1.0;
    double segment_last_ms_ = -1.0;
    std::size_t frames_written_ = 0;
    std::size_t dropped_at_segment_start_ = 0;
    unsigned long file_sequence_ = 0;

    std::atomic<std::size_t> completed_segments_{0};
    std::atomic<std::size_t> dropped_frames_{0};
};
