#include "video_io.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <opencv2/highgui.hpp>

#include "log/logger.h"

// All six functions moved verbatim from main.cpp (spec main-cpp-refactor).
// Every LOG_INFO/LOG_WARN "cam" string is preserved character-for-character.

// Open (or reopen) a VideoCapture from cfg. Returns true if opened. Applies
// the same USB tuning (MJPG/size/fps/buffer) the main path used.
bool open_capture(cv::VideoCapture& cap, const CamConfig& cfg) {
    if (cap.isOpened()) cap.release();
    if (cfg.is_stream) {
        cap.open(cfg.pipeline, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) return false;
    } else {
        cap.open(cfg.cam_id, cv::CAP_V4L2);
        if (!cap.isOpened()) return false;
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
        cap.set(cv::CAP_PROP_FRAME_WIDTH,  cfg.cam_w);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.cam_h);
        cap.set(cv::CAP_PROP_FPS,          cfg.cam_fps);
        cap.set(cv::CAP_PROP_BUFFERSIZE,   1);
    }
    return true;
}

// Capture thread. Reads frames into the latest-frame slot. On a run of failed
// reads (source dropped — RTSP disconnect, USB unplug), it tears the capture
// down and reopens it with exponential backoff instead of spinning forever on
// a dead handle (DEVELOPMENT_PLAN: RTSP reconnect).
void capture_worker(cv::VideoCapture* cap, FrameSlot* slot,
                    CamConfig cfg) {
    cv::Mat local;
    int  consecutive_fails = 0;
    int  backoff_ms        = cfg.backoff_min_ms;

    auto should_stop = [&]() {
        std::lock_guard<std::mutex> lk(slot->mtx);
        return slot->stop;
    };

    while (true) {
        if (should_stop()) return;

        if (cap->read(local) && !local.empty()) {
            // Healthy frame: reset failure tracking and publish it.
            consecutive_fails = 0;
            backoff_ms        = cfg.backoff_min_ms;
            {
                std::lock_guard<std::mutex> lk(slot->mtx);
                if (slot->stop) return;
                local.copyTo(slot->latest);
                slot->seq++;
            }
            slot->cv_new.notify_one();
            continue;
        }

        // Read failed. Tolerate brief hiccups; after a run of failures assume
        // the source is gone and reconnect.
        if (++consecutive_fails < cfg.fail_reopen_threshold) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        LOG_WARN("cam", "source read failing (%d consecutive) — reconnecting",
                 consecutive_fails);

        // Reconnect loop with exponential backoff, until success or stop.
        while (!should_stop()) {
            // Sleep the backoff in small slices so a shutdown request during a
            // long backoff still tears down promptly (join won't block ~10s).
            for (int slept = 0; slept < backoff_ms; slept += 100) {
                if (should_stop()) return;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min(100, backoff_ms - slept)));
            }
            if (should_stop()) return;

            if (open_capture(*cap, cfg)) {
                LOG_INFO("cam", "reconnected to source after %d ms backoff",
                         backoff_ms);
                consecutive_fails = 0;
                backoff_ms        = cfg.backoff_min_ms;
                break;
            }
            LOG_WARN("cam", "reconnect attempt failed (backoff %d ms)", backoff_ms);
            backoff_ms = std::min(backoff_ms * 2, cfg.backoff_max_ms);
        }
    }
}

void display_worker(const char* win_name, DisplaySlot* slot,
                    std::atomic<bool>* stop_flag,
                    FrameSlot* capture_slot,
                    bool fullscreen) {
    if (fullscreen) {
        cv::namedWindow(win_name, cv::WINDOW_NORMAL);
        cv::setWindowProperty(win_name, cv::WND_PROP_FULLSCREEN,
                              cv::WINDOW_FULLSCREEN);
    } else {
        cv::namedWindow(win_name, cv::WINDOW_AUTOSIZE);
    }
    cv::Mat local;
    uint64_t last_seq = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(slot->mtx);
            slot->cv_new.wait(lk, [&]{
                return slot->seq != last_seq || slot->stop;
            });
            if (slot->stop) break;
            slot->latest.copyTo(local);
            last_seq = slot->seq;
        }
        cv::imshow(win_name, local);
        int k = cv::waitKey(1) & 0xFF;
        if (k == 'q' || k == 27) {
            stop_flag->store(true);
            if (capture_slot) capture_slot->cv_new.notify_all();
            std::lock_guard<std::mutex> lk(slot->mtx);
            slot->last_key = k;
            break;
        }
    }
    cv::destroyWindow(win_name);
}

std::string build_gst_pipeline(const std::string& url, int latency_ms) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "uridecodebin uri=%s buffer-duration=%d000000 ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=BGR ! "
        "appsink drop=true sync=false max-buffers=1",
        url.c_str(), latency_ms);
    return std::string(buf);
}

bool is_stream_source(const std::string& s) {
    return s.rfind("rtsp://", 0) == 0 ||
           s.rfind("http://", 0) == 0 ||
           s.rfind("https://", 0) == 0 ||
           s.rfind("file://", 0) == 0 ||
           s.find(".mp4") != std::string::npos ||
           s.find(".mkv") != std::string::npos ||
           s.find(".avi") != std::string::npos;
}
