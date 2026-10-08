#include "app_config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>

namespace {

bool parse_int_in_range(const char* value, int minimum, int maximum, int& result) {
    if (!value) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno || end == value || *end != '\0' || parsed < minimum || parsed > maximum)
        return false;
    result = static_cast<int>(parsed);
    return true;
}

bool parse_double_in_range(const char* value, double minimum, double maximum, double& result) {
    if (!value) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value, &end);
    if (errno || end == value || *end != '\0' || !std::isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
        return false;
    result = parsed;
    return true;
}

}  // namespace

// Moved verbatim from main.cpp (spec main-cpp-refactor). Usage text is
// byte-for-byte identical to the Baseline and emitted to stderr.
void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [detect.nb] [cam_id] [options]\n"
        "  positional args (optional):\n"
        "    detect.nb   SCRFD .nb path (default: model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb)\n"
        "    cam_id      /dev/videoN index (default: 0)\n"
        "  options:\n"
        "    --frames N            Run N frames then exit with benchmark summary\n"
        "    --recog-model PATH    Enable face recognition, path to recog .nb\n"
        "    --recog-dim N         Embedding dimension (default 512)\n"
        "    --recog-bgr           Feed BGR to recog (default RGB swap on)\n"
        "    --face-db PATH        Load .fdb identity DB (TEST/DEV mode only,\n"
        "                          no floor/language/audit — NOT for real cabin)\n"
        "    --resident-db PATH    SQLite resident DB (OPERATIONAL mode: floor,\n"
        "                          greeting, audit log). Takes precedence over --face-db.\n"
        "    --resident-reload-ms N Poll resident/embedding changes (default 1000; 0=off)\n"
        "    --cabin-id N          Cabin id recorded in match_events (default 1)\n"
        "    --confirm-streak N    Frames of consecutive match to confirm (default 5)\n"
        "    --cooldown-ms N       Per-resident cooldown between events (default 3000)\n"
        "    --unknown-after-ms N  Present-but-unmatched timeout to log unknown (default 2000)\n"
        "    --match-thr F         Cosine similarity threshold (default 0.35)\n"
        "    --person-model PATH   Enable YOLO person detection + tracker\n"
        "    --person-thr F        High person score threshold (default 0.5)\n"
        "    --person-low-thr F    Low ByteTrack score threshold (default 0.1)\n"
        "    --person-every N      Only run YOLO every N frames (default 1)\n"
        "    --track-iou F         Min IoU for track association (default 0.3)\n"
        "    --track-max-miss N    Kill track after N missed frames (default 30)\n"
        "    --track-lost-ms N     ByteTrack removal timeout (default 1200)\n"
        "    --tracker-mode MODE   greedy|byte (default greedy during rollout)\n"
        "    --track-conflict-recovery  Enable admin-only known-identity probes\n"
        "    --record-dir DIR      Record raw person-presence clips (disabled by default)\n"
        "    --record-pre-ms N     Include N ms before person confirmation (default 1000)\n"
        "    --record-empty-ms N   Stop after N ms without a tracked person (default 3000)\n"
        "    --record-fps F        MJPEG/AVI output FPS (default 15)\n"
        "    --record-width N      Output width; 0 keeps source width (default 960)\n"
        "    --recog-retry N       Re-verify recognition every N frames (default 90)\n"
        "    --ui-scale F          Overlay text/box scale. 0=auto(frame.h/480), 2.0=fix 2x\n"
        "    --source URL          RTSP/HTTP/file source (GStreamer)\n"
        "    --gst-pipeline STR    Custom GStreamer pipeline\n"
        "    --gst-latency MS      RTSP jitter buffer latency (default 100ms)\n"
        "    --reconnect-min-ms N  Reconnect backoff floor (default 500)\n"
        "    --reconnect-max-ms N  Reconnect backoff ceiling (default 10000)\n"
        "    --edge-socket PATH    UDS path to Edge Core (e.g. /run/elev_edge_core.sock or\n"
        "                          @elev_edge_core.sock). Empty = disabled (default).\n"
        "    --iot-ws URL          IoT Core Socket.IO endpoint (ws://...; disabled by default)\n"
        "    --iot-device-code ID  IoT deviceCode (default 2026010900100098)\n"
        "    --iot-floor-address A Floor-write address (default 8193-2, elevator/slave 2)\n"
        "    --iot-request-id ID   Client/command requestId (default ai-core)\n"
        "    --floor-session-empty-ms N  End cabin visit after N ms empty (default 5000)\n"
        "    --floor-command-ttl-ms N    Drop unsent floor call after N ms (default 5000)\n"
        "    --floor-ack-timeout-ms N    Wait N ms for IoT command ACK (default 3000)\n"
        "    --floor-max-per-visit N     Maximum distinct floor calls per visit (default 16)\n"
        "    --no-preview          Disable OpenCV preview window (headless mode)\n"
        "    --windowed / --fullscreen\n"
        "    --log-level L         trace|debug|info|warn|error (default info)\n"
        "    --log-dir DIR         log file directory (default /var/log/face-cabin)\n",
        prog);
}

// Moved verbatim from main.cpp (spec main-cpp-refactor). The argv loop is
// identical to the Baseline: at most 2 positionals (det model path, then cam
// id), the sv() value-flag helper requiring i+1 < argc, atoi/atof coercion,
// unknown/malformed-flag silent ignore. The sole behavioral-equivalent change
// is that -h/--help reports help_requested instead of `return 0` inline, so
// main keeps exit-code ownership (usage on stderr + exit 0 unchanged).
ParseResult parse_args(int argc, char** argv, AppConfig& cfg) {
    ParseResult res;
    int positional_idx = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' && positional_idx < 2) {
            if (positional_idx == 0) cfg.det_model_path = a;
            else if (positional_idx == 1) cfg.cam_id = std::atoi(a);
            ++positional_idx;
            continue;
        }
        auto sv = [&](const char* k) { return std::strcmp(a, k) == 0 && i + 1 < argc; };
        if      (sv("--frames"))         cfg.max_frames = std::atoi(argv[++i]);
        else if (sv("--recog-model"))    cfg.recog_model_path = argv[++i];
        else if (sv("--recog-dim"))      cfg.recog_dim = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--recog-bgr") == 0) cfg.recog_rgb = false;
        else if (sv("--face-db"))        cfg.face_db_path = argv[++i];
        else if (sv("--resident-db"))    cfg.resident_db_path = argv[++i];
        else if (std::strcmp(a, "--resident-reload-ms") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--resident-reload-ms requires a non-negative integer\n");
                res.invalid_args = true; return res;
            }
            const char* value = argv[++i];
            char* end = nullptr; errno = 0;
            long parsed = std::strtol(value, &end, 10);
            if (errno || end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
                std::fprintf(stderr, "invalid --resident-reload-ms: %s\n", value);
                res.invalid_args = true; return res;
            }
            cfg.resident_reload_ms = static_cast<int>(parsed);
        }
        else if (sv("--cabin-id"))       cfg.cabin_id = std::atoi(argv[++i]);
        else if (sv("--confirm-streak")) { cfg.confirm_streak = std::max(1, std::atoi(argv[++i])); cfg.passed.confirm_streak = true; }
        else if (sv("--cooldown-ms"))    { cfg.cooldown_ms = std::atof(argv[++i]); cfg.passed.cooldown = true; }
        else if (sv("--unknown-after-ms")) { cfg.unknown_after_ms = std::atof(argv[++i]); cfg.passed.unknown_after = true; }
        else if (sv("--match-thr"))      { cfg.match_threshold = (float)std::atof(argv[++i]); cfg.passed.match_thr = true; }
        else if (sv("--person-model"))   cfg.person_model_path = argv[++i];
        else if (sv("--person-thr"))     cfg.person_thr = (float)std::atof(argv[++i]);
        else if (sv("--person-low-thr")) cfg.person_low_thr = (float)std::atof(argv[++i]);
        else if (sv("--person-every"))   cfg.person_every = std::max(1, std::atoi(argv[++i]));
        else if (sv("--track-iou"))      cfg.track_iou = (float)std::atof(argv[++i]);
        else if (sv("--track-max-miss")) cfg.track_max_miss = std::atoi(argv[++i]);
        else if (sv("--track-lost-ms"))  cfg.track_lost_ms = std::atoi(argv[++i]);
        else if (sv("--tracker-mode")) {
            const char* mode = argv[++i];
            if (std::strcmp(mode, "greedy") == 0) cfg.tracker_mode = TrackerMode::Greedy;
            else if (std::strcmp(mode, "byte") == 0) cfg.tracker_mode = TrackerMode::Byte;
            else {
                std::fprintf(stderr, "invalid --tracker-mode: %s (expected greedy|byte)\n", mode);
                res.invalid_args = true; return res;
            }
        }
        else if (std::strcmp(a, "--track-conflict-recovery") == 0) cfg.track_conflict_recovery = true;
        else if (std::strcmp(a, "--record-dir") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
                std::fprintf(stderr, "--record-dir requires a non-empty directory\n");
                res.invalid_args = true; return res;
            }
            cfg.record_dir = argv[++i];
        }
        else if (std::strcmp(a, "--record-pre-ms") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 0, INT_MAX, cfg.record_pre_ms)) {
                std::fprintf(stderr, "--record-pre-ms requires a non-negative integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--record-empty-ms") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 1, INT_MAX, cfg.record_empty_ms)) {
                std::fprintf(stderr, "--record-empty-ms requires a positive integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--record-fps") == 0) {
            if (i + 1 >= argc || !parse_double_in_range(argv[i + 1], 1.0, 120.0, cfg.record_fps)) {
                std::fprintf(stderr, "--record-fps requires a number in [1, 120]\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--record-width") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 0, 7680, cfg.record_width)) {
                std::fprintf(stderr, "--record-width requires an integer in [0, 7680]\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (sv("--recog-retry"))    cfg.recog_retry = std::atoi(argv[++i]);
        else if (sv("--ui-scale"))       cfg.ui_scale_override = (float)std::atof(argv[++i]);
        else if (std::strcmp(a, "--no-preview") == 0) cfg.preview = false;
        else if (sv("--source"))         { cfg.source_url = argv[++i]; cfg.passed.source = true; }
        else if (sv("--gst-pipeline"))   cfg.custom_pipeline = argv[++i];
        else if (sv("--edge-socket"))    cfg.edge_socket = argv[++i];
        else if (sv("--iot-ws"))         cfg.iot_ws = argv[++i];
        else if (sv("--face-event-ws"))  cfg.iot_ws = argv[++i]; // migration alias
        else if (sv("--iot-device-code")) cfg.iot_device_code = argv[++i];
        else if (sv("--iot-floor-address")) cfg.iot_floor_address = argv[++i];
        else if (sv("--iot-request-id")) cfg.iot_request_id = argv[++i];
        else if (std::strcmp(a, "--floor-session-empty-ms") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 1, INT_MAX,
                                                       cfg.floor_session_empty_ms)) {
                std::fprintf(stderr, "--floor-session-empty-ms requires a positive integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--floor-command-ttl-ms") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 1, INT_MAX,
                                                       cfg.floor_command_ttl_ms)) {
                std::fprintf(stderr, "--floor-command-ttl-ms requires a positive integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--floor-ack-timeout-ms") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 1, INT_MAX,
                                                       cfg.floor_ack_timeout_ms)) {
                std::fprintf(stderr, "--floor-ack-timeout-ms requires a positive integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (std::strcmp(a, "--floor-max-per-visit") == 0) {
            if (i + 1 >= argc || !parse_int_in_range(argv[i + 1], 1, INT_MAX,
                                                       cfg.floor_max_commands_per_visit)) {
                std::fprintf(stderr, "--floor-max-per-visit requires a positive integer\n");
                res.invalid_args = true; return res;
            }
            ++i;
        }
        else if (sv("--gst-latency"))    { cfg.gst_latency_ms = std::atoi(argv[++i]); cfg.passed.gst_latency = true; }
        else if (sv("--reconnect-min-ms")) { cfg.reconnect_min_ms = std::atoi(argv[++i]); cfg.passed.reconnect_min = true; }
        else if (sv("--reconnect-max-ms")) { cfg.reconnect_max_ms = std::atoi(argv[++i]); cfg.passed.reconnect_max = true; }
        else if (std::strcmp(a, "--windowed") == 0)   cfg.fullscreen = false;
        else if (std::strcmp(a, "--fullscreen") == 0) cfg.fullscreen = true;
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            res.help_requested = true;
            return res;
        }
    }
    if (!std::isfinite(cfg.person_low_thr) || !std::isfinite(cfg.person_thr) ||
        cfg.person_low_thr < 0.0f || cfg.person_low_thr >= cfg.person_thr || cfg.person_thr > 1.0f) {
        std::fprintf(stderr, "invalid person thresholds: require 0 <= --person-low-thr < --person-thr <= 1\n");
        res.invalid_args = true;
    } else if (!std::isfinite(cfg.track_iou) || cfg.track_iou < 0.0f || cfg.track_iou > 1.0f) {
        std::fprintf(stderr, "invalid --track-iou: require 0 <= value <= 1\n");
        res.invalid_args = true;
    } else if (cfg.track_lost_ms <= 250) {
        std::fprintf(stderr, "invalid --track-lost-ms: must exceed lost grace 250ms\n");
        res.invalid_args = true;
    } else if (cfg.floor_session_empty_ms < cfg.track_lost_ms) {
        std::fprintf(stderr, "invalid --floor-session-empty-ms: must be >= --track-lost-ms\n");
        res.invalid_args = true;
    } else if (cfg.record_dir && !cfg.person_model_path) {
        std::fprintf(stderr, "--record-dir requires --person-model for person presence detection\n");
        res.invalid_args = true;
    } else if (cfg.edge_socket && cfg.iot_ws) {
        std::fprintf(stderr, "--edge-socket and --iot-ws cannot be enabled together\n");
        res.invalid_args = true;
    }
    return res;
}
