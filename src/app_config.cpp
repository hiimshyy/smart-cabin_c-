#include "app_config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

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
        "    --cabin-id N          Cabin id recorded in match_events (default 1)\n"
        "    --confirm-streak N    Frames of consecutive match to confirm (default 5)\n"
        "    --cooldown-ms N       Per-resident cooldown between events (default 3000)\n"
        "    --unknown-after-ms N  Present-but-unmatched timeout to log unknown (default 2000)\n"
        "    --match-thr F         Cosine similarity threshold (default 0.35)\n"
        "    --person-model PATH   Enable YOLO person detection + tracker\n"
        "    --person-thr F        Person score threshold (default 0.5)\n"
        "    --person-every N      Only run YOLO every N frames (default 1)\n"
        "    --track-iou F         Min IoU for track association (default 0.3)\n"
        "    --track-max-miss N    Kill track after N missed frames (default 30)\n"
        "    --recog-retry N       Re-verify recognition every N frames (default 90)\n"
        "    --ui-scale F          Overlay text/box scale. 0=auto(frame.h/480), 2.0=fix 2x\n"
        "    --source URL          RTSP/HTTP/file source (GStreamer)\n"
        "    --gst-pipeline STR    Custom GStreamer pipeline\n"
        "    --gst-latency MS      RTSP jitter buffer latency (default 100ms)\n"
        "    --reconnect-min-ms N  Reconnect backoff floor (default 500)\n"
        "    --reconnect-max-ms N  Reconnect backoff ceiling (default 10000)\n"
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
        else if (sv("--cabin-id"))       cfg.cabin_id = std::atoi(argv[++i]);
        else if (sv("--confirm-streak")) cfg.confirm_streak = std::max(1, std::atoi(argv[++i]));
        else if (sv("--cooldown-ms"))    cfg.cooldown_ms = std::atof(argv[++i]);
        else if (sv("--unknown-after-ms")) cfg.unknown_after_ms = std::atof(argv[++i]);
        else if (sv("--match-thr"))      cfg.match_threshold = (float)std::atof(argv[++i]);
        else if (sv("--person-model"))   cfg.person_model_path = argv[++i];
        else if (sv("--person-thr"))     cfg.person_thr = (float)std::atof(argv[++i]);
        else if (sv("--person-every"))   cfg.person_every = std::max(1, std::atoi(argv[++i]));
        else if (sv("--track-iou"))      cfg.track_iou = (float)std::atof(argv[++i]);
        else if (sv("--track-max-miss")) cfg.track_max_miss = std::atoi(argv[++i]);
        else if (sv("--recog-retry"))    cfg.recog_retry = std::atoi(argv[++i]);
        else if (sv("--ui-scale"))       cfg.ui_scale_override = (float)std::atof(argv[++i]);
        else if (sv("--source"))         cfg.source_url = argv[++i];
        else if (sv("--gst-pipeline"))   cfg.custom_pipeline = argv[++i];
        else if (sv("--gst-latency"))    cfg.gst_latency_ms = std::atoi(argv[++i]);
        else if (sv("--reconnect-min-ms")) cfg.reconnect_min_ms = std::atoi(argv[++i]);
        else if (sv("--reconnect-max-ms")) cfg.reconnect_max_ms = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--windowed") == 0)   cfg.fullscreen = false;
        else if (std::strcmp(a, "--fullscreen") == 0) cfg.fullscreen = true;
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            res.help_requested = true;
            return res;
        }
    }
    return res;
}
