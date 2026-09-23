#pragma once

// Application configuration parsed from the CLI (spec main-cpp-refactor,
// Config_Module). Every field's default is byte-for-byte identical to the
// Baseline main.cpp inline defaults. The const char* paths point into argv
// (no ownership), exactly as the Baseline.
struct AppConfig {
    const char* det_model_path    = "model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb";
    const char* recog_model_path  = nullptr;
    const char* face_db_path      = nullptr;
    const char* resident_db_path  = nullptr;
    const char* person_model_path = nullptr;
    const char* source_url        = nullptr;
    const char* custom_pipeline   = nullptr;

    int    cam_id           = 0;
    int    max_frames       = 0;
    int    recog_dim        = 512;
    int    gst_latency_ms   = 100;
    int    reconnect_min_ms = 500;      // RTSP reconnect backoff floor
    int    reconnect_max_ms = 10000;    // RTSP reconnect backoff ceiling
    int    person_every     = 1;
    int    track_max_miss   = 30;
    int    recog_retry      = 90;
    int    cabin_id         = 1;
    int    confirm_streak   = 5;
    double cooldown_ms       = 3000.0;
    double unknown_after_ms  = 2000.0;
    float  track_iou         = 0.3f;
    float  person_thr        = 0.5f;
    float  ui_scale_override = 0.0f;   // 0 = auto (scale by frame.rows / 480)
    bool   recog_rgb         = true;
    bool   fullscreen        = true;
    float  match_threshold   = 0.35f;
};

// Prints the Baseline usage text (byte-for-byte) to stderr.
void print_usage(const char* prog);

// Parse argv into cfg using the exact Baseline argv loop. On -h/--help the
// usage is printed here (to stderr) and help_requested is set true; the caller
// (main) then returns exit code 0, preserving exit-code ownership in main.
struct ParseResult { bool help_requested = false; };
ParseResult parse_args(int argc, char** argv, AppConfig& cfg);
