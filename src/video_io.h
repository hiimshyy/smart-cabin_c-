#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

// Threaded video capture + display (spec main-cpp-refactor, Video_IO_Module).
// All structs and functions are moved verbatim from main.cpp; the
// synchronization fields (mutex/condition_variable/seq/stop) are unchanged so
// the producer/consumer handshake and thus threading behavior are identical.

// -------- Capture thread (latest-frame slot) ----------
struct FrameSlot {
    std::mutex               mtx;
    std::condition_variable  cv_new;
    cv::Mat                  latest;
    uint64_t                 seq  = 0;
    bool                     stop = false;
};

// All the parameters needed to (re)open the video source, so the capture
// thread can rebuild a dead VideoCapture on its own (RTSP reconnect).
struct CamConfig {
    bool        is_stream   = false;   // GStreamer/RTSP vs V4L2 USB
    std::string pipeline;              // full GStreamer pipeline (stream mode)
    int         cam_id      = 0;       // /dev/videoN (USB mode)
    int         cam_w       = 640;
    int         cam_h       = 480;
    int         cam_fps     = 30;
    // Exponential backoff bounds for reconnect (milliseconds).
    int         backoff_min_ms = 500;
    int         backoff_max_ms = 10000;
    // How many consecutive failed reads before we tear down + reopen.
    int         fail_reopen_threshold = 30;   // ~0.15-1s depending on source
};

// -------- Display thread ----------
struct DisplaySlot {
    std::mutex               mtx;
    std::condition_variable  cv_new;
    cv::Mat                  latest;
    uint64_t                 seq  = 0;
    bool                     stop = false;
    int                      last_key = -1;
};

// Open (or reopen) a VideoCapture from cfg. Returns true if opened.
bool        open_capture(cv::VideoCapture& cap, const CamConfig& cfg);

// Capture thread. Reads frames into the latest-frame slot; reconnects with
// exponential backoff when the source drops.
void        capture_worker(cv::VideoCapture* cap, FrameSlot* slot, CamConfig cfg);

// Display thread. imshow loop + q/ESC handling. stop_flag stays owned by main.
void        display_worker(const char* win_name, DisplaySlot* slot,
                           std::atomic<bool>* stop_flag,
                           FrameSlot* capture_slot, bool fullscreen);

std::string build_gst_pipeline(const std::string& url, int latency_ms);
bool        is_stream_source(const std::string& s);
