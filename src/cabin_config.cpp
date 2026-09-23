#include "cabin_config.h"

#include <algorithm>
#include <cctype>

#include "log/logger.h"

// --------------------------------------------------------------------------
// camera_urls parsing (R2.3) + scheme validation (R4.6, Nhóm C anti-injection)
// --------------------------------------------------------------------------

std::string parse_first_camera_url(const std::string& s) {
    // Minimal: find the first double-quoted token. camera_urls is a JSON array
    // of strings like ["rtsp://host/stream", ...]; we only need element [0].
    size_t open = s.find('"');
    if (open == std::string::npos) return "";
    size_t close = s.find('"', open + 1);
    if (close == std::string::npos) return "";
    return s.substr(open + 1, close - open - 1);
}

bool is_allowed_camera_url(const std::string& url) {
    // Allowed: rtsp:// http:// https://. Everything else (empty, a raw
    // GStreamer pipeline, file paths, cam://N) is NOT a web-settable stream
    // source. A pipeline string typically contains spaces / '!' — reject.
    if (url.rfind("rtsp://", 0) == 0)  return true;
    if (url.rfind("http://", 0) == 0)  return true;
    if (url.rfind("https://", 0) == 0) return true;
    return false;
}

// --------------------------------------------------------------------------
// resolve_cabin_config: precedence CLI > DB > default, then validate/clamp
// --------------------------------------------------------------------------

namespace {

template <typename T>
T clamp_lo(const char* field, T v, T lo) {
    if (v < lo) {
        LOG_WARN("cfg", "%s=%g below min %g — clamped", field, (double)v, (double)lo);
        return lo;
    }
    return v;
}

}  // namespace

CabinConfig resolve_cabin_config(const CliOverrides& cli,
                                 const CabinRow& db,
                                 const CabinConfig& def) {
    CabinConfig c;
    const bool have_db = db.found;

    // ---- Per-field precedence: CLI (if passed) > DB (if row) > default ----
    // gst_latency_ms
    c.gst_latency_ms = cli.has_gst_latency ? cli.gst_latency_val
                     : have_db             ? db.gst_latency_ms
                                           : def.gst_latency_ms;
    // match_thr
    c.match_thr = cli.has_match_thr ? cli.match_thr_val
                : have_db           ? db.match_thr
                                    : def.match_thr;
    // confirm_streak
    c.confirm_streak = cli.has_confirm_streak ? cli.confirm_streak_val
                     : have_db                ? db.confirm_streak
                                              : def.confirm_streak;
    // cooldown_ms
    c.cooldown_ms = cli.has_cooldown ? cli.cooldown_val
                  : have_db          ? (double)db.cooldown_ms
                                     : def.cooldown_ms;
    // unknown_after_ms
    c.unknown_after_ms = cli.has_unknown_after ? cli.unknown_after_val
                       : have_db               ? (double)db.unknown_after_ms
                                               : def.unknown_after_ms;
    // reconnect min/max
    c.reconnect_min_ms = cli.has_reconnect_min ? cli.reconnect_min_val
                       : have_db               ? db.reconnect_min_ms
                                               : def.reconnect_min_ms;
    c.reconnect_max_ms = cli.has_reconnect_max ? cli.reconnect_max_val
                       : have_db               ? db.reconnect_max_ms
                                               : def.reconnect_max_ms;
    // floors (DB-only; no CLI equivalent)
    c.floors_min = have_db ? db.floors_min : def.floors_min;
    c.floors_max = have_db ? db.floors_max : def.floors_max;
    c.elevator_endpoint = have_db ? db.elevator_endpoint : def.elevator_endpoint;

    // ---- Camera source: CLI --source > DB camera_urls[0] > default -------
    std::string url;
    if (cli.has_source) {
        url = cli.source_val;               // dev override (may be any scheme)
    } else if (have_db) {
        url = parse_first_camera_url(db.camera_urls);
        if (!url.empty() && !is_allowed_camera_url(url)) {
            LOG_WARN("cfg", "cabin camera_urls[0] has disallowed scheme — "
                            "falling back to USB cam");
            url = "";
        }
    } else {
        url = def.rtsp_url;
    }
    // A CLI --source is trusted as-is (dev); a DB URL was scheme-checked above.
    c.rtsp_url   = url;
    c.use_stream = !url.empty();

    // ---- Validate / clamp (R4) -------------------------------------------
    if (c.match_thr < 0.05f || c.match_thr > 0.95f) {
        LOG_WARN("cfg", "match_thr=%.3f out of [0.05,0.95] — clamped", c.match_thr);
        c.match_thr = std::min(0.95f, std::max(0.05f, c.match_thr));
    }
    c.confirm_streak   = clamp_lo("confirm_streak", c.confirm_streak, 1);
    c.cooldown_ms      = clamp_lo("cooldown_ms", c.cooldown_ms, 0.0);
    c.unknown_after_ms = clamp_lo("unknown_after_ms", c.unknown_after_ms, 0.0);
    c.gst_latency_ms   = clamp_lo("gst_latency_ms", c.gst_latency_ms, 0);
    c.reconnect_min_ms = clamp_lo("reconnect_min_ms", c.reconnect_min_ms, 1);
    if (c.reconnect_min_ms > c.reconnect_max_ms) {
        LOG_WARN("cfg", "reconnect_min_ms=%d > max=%d — swapping",
                 c.reconnect_min_ms, c.reconnect_max_ms);
        std::swap(c.reconnect_min_ms, c.reconnect_max_ms);
    }
    if (c.floors_min > c.floors_max) {
        LOG_WARN("cfg", "floors_min=%d > floors_max=%d — using defaults %d..%d",
                 c.floors_min, c.floors_max, def.floors_min, def.floors_max);
        c.floors_min = def.floors_min;
        c.floors_max = def.floors_max;
    }
    return c;
}

// --------------------------------------------------------------------------
// validate_cabin_patch: shared REST-PATCH validation (R4.6, R4.7)
// --------------------------------------------------------------------------
std::vector<std::string> validate_cabin_patch(const CabinPatch& p) {
    std::vector<std::string> errs;

    if (p.has_camera_urls) {
        // Must be a JSON array whose first element is an allowed scheme. A raw
        // GStreamer pipeline / odd scheme is rejected (anti command-injection).
        std::string url = parse_first_camera_url(p.camera_urls);
        if (url.empty()) {
            errs.push_back("camera_urls: empty or not a JSON array of URLs");
        } else if (!is_allowed_camera_url(url)) {
            errs.push_back("camera_urls[0]: scheme must be rtsp/http/https "
                           "(raw GStreamer pipelines are not allowed)");
        }
    }
    if (p.has_match_thr && (p.match_thr < 0.05f || p.match_thr > 0.95f))
        errs.push_back("match_thr: must be within [0.05, 0.95]");
    if (p.has_confirm_streak && p.confirm_streak < 1)
        errs.push_back("confirm_streak: must be >= 1");
    if (p.has_cooldown_ms && p.cooldown_ms < 0)
        errs.push_back("cooldown_ms: must be >= 0");
    if (p.has_unknown_after_ms && p.unknown_after_ms < 0)
        errs.push_back("unknown_after_ms: must be >= 0");
    if (p.has_gst_latency_ms && p.gst_latency_ms < 0)
        errs.push_back("gst_latency_ms: must be >= 0");
    if (p.has_reconnect_min_ms && p.reconnect_min_ms < 1)
        errs.push_back("reconnect_min_ms: must be >= 1");
    // If both bounds present, min must be <= max.
    if (p.has_reconnect_min_ms && p.has_reconnect_max_ms &&
        p.reconnect_min_ms > p.reconnect_max_ms)
        errs.push_back("reconnect_min_ms: must be <= reconnect_max_ms");
    if (p.has_floors_min && p.has_floors_max && p.floors_min > p.floors_max)
        errs.push_back("floors_min: must be <= floors_max");

    return errs;
}
