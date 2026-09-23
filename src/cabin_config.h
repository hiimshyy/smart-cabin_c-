#pragma once
// Cabin runtime config resolution + validation (spec cabin-runtime-config).
//
// Effective config = precedence CLI > DB > default, then validate/clamp. This
// module is the ONLY place that turns a raw cabins row into the values the
// pipeline consumes, and it is also called by the REST PATCH path so the app
// and the API never validate differently. No NPU/OpenCV dependency — testable
// with g++ + sqlite3 alone.

#include <string>
#include <vector>

#include "resident_db.h"   // CabinRow, CabinPatch

// Config already resolved + validated, ready to build CamConfig /
// InteractionConfig / match_threshold in main.
struct CabinConfig {
    std::string rtsp_url;               // camera_urls[0]; "" => use USB cam
    bool        use_stream       = false;
    int         gst_latency_ms   = 100;
    float       match_thr        = 0.35f;
    int         confirm_streak   = 5;
    double      cooldown_ms      = 3000.0;
    double      unknown_after_ms = 2000.0;
    int         reconnect_min_ms = 500;
    int         reconnect_max_ms = 10000;
    int         floors_min       = 1;
    int         floors_max       = 30;
    std::string elevator_endpoint;
};

// Flags marking which Nhóm A params the user ACTUALLY passed on the CLI, so a
// CLI value overrides DB only when truly supplied (not when it merely equals a
// default). Values are read only when the matching flag is true.
struct CliOverrides {
    bool        has_source        = false;  std::string source_val;      // rtsp url or ""
    bool        has_gst_latency   = false;  int    gst_latency_val   = 100;
    bool        has_match_thr     = false;  float  match_thr_val     = 0.35f;
    bool        has_confirm_streak= false;  int    confirm_streak_val= 5;
    bool        has_cooldown      = false;  double cooldown_val      = 3000.0;
    bool        has_unknown_after = false;  double unknown_after_val = 2000.0;
    bool        has_reconnect_min = false;  int    reconnect_min_val = 500;
    bool        has_reconnect_max = false;  int    reconnect_max_val = 10000;
};

// Parse a camera_urls JSON array string, returning the first element (v1 uses
// one camera). Minimal parser (no JSON lib): finds the first quoted string
// inside [...]. Returns "" if empty/unparseable.
std::string parse_first_camera_url(const std::string& camera_urls_json);

// True if `url` is an accepted RTSP-source scheme (rtsp/http/https). A raw
// GStreamer pipeline or any other scheme is rejected (Nhóm C, anti-injection).
bool is_allowed_camera_url(const std::string& url);

// Resolve effective config: precedence CLI > DB (when db.found) > def, then
// validate/clamp per R4 (logs a warning on each clamp). `def` supplies the
// compile-in defaults.
CabinConfig resolve_cabin_config(const CliOverrides& cli,
                                 const CabinRow& db,
                                 const CabinConfig& def);

// Validate a REST PATCH payload before it is written to the DB. Returns the
// list of human-readable field errors (empty => valid). Rejects out-of-range
// values, bad camera URL schemes, and raw pipeline strings (R4.6, R4.7). The
// caller (REST layer) maps a non-empty result to HTTP 400.
std::vector<std::string> validate_cabin_patch(const CabinPatch& patch);
