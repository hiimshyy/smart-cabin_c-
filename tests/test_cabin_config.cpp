// Unit tests for cabin_config (spec cabin-runtime-config, Task 3.6).
// Covers: precedence CLI>DB>default (3 combos), validate/clamp per constraint,
// camera_urls parse + scheme rejection, validate_cabin_patch.
// No NPU/OpenCV — g++ + sqlite3 only.
#include "cabin_config.h"
#include <cstdio>

static int g_checks = 0;
static int g_fail   = 0;
static void check(bool ok, const char* msg) {
    ++g_checks;
    if (!ok) { ++g_fail; printf("  FAIL: %s\n", msg); }
}

int main() {
    CabinConfig def;   // compile-in defaults

    // ---- Precedence combo 1: DB found, no CLI -> DB wins ----
    {
        CabinRow db; db.found = true;
        db.match_thr = 0.42f; db.cooldown_ms = 1500;
        db.camera_urls = "[\"rtsp://db/cam\"]";
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.match_thr == 0.42f, "combo1: match_thr from DB");
        check(c.cooldown_ms == 1500, "combo1: cooldown from DB");
        check(c.use_stream && c.rtsp_url == "rtsp://db/cam", "combo1: rtsp from DB");
    }

    // ---- Precedence combo 2: CLI passed -> CLI overrides DB ----
    {
        CabinRow db; db.found = true; db.match_thr = 0.42f;
        CliOverrides cli;
        cli.has_match_thr = true; cli.match_thr_val = 0.55f;
        cli.has_source = true;    cli.source_val = "rtsp://cli/cam";
        CabinConfig c = resolve_cabin_config(cli, db, def);
        check(c.match_thr == 0.55f, "combo2: CLI match_thr overrides DB");
        check(c.rtsp_url == "rtsp://cli/cam", "combo2: CLI source overrides DB");
    }

    // ---- Precedence combo 3: no DB row, no CLI -> defaults ----
    {
        CabinRow db; db.found = false;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.match_thr == def.match_thr, "combo3: default match_thr");
        check(!c.use_stream, "combo3: no stream -> USB");
    }

    // ---- Validate / clamp ----
    {
        CabinRow db; db.found = true; db.match_thr = 5.0f;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.match_thr == 0.95f, "clamp: match_thr high -> 0.95");
    }
    {
        CabinRow db; db.found = true; db.match_thr = 0.0f;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.match_thr == 0.05f, "clamp: match_thr low -> 0.05");
    }
    {
        CabinRow db; db.found = true; db.confirm_streak = 0;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.confirm_streak == 1, "clamp: confirm_streak -> 1");
    }
    {
        CabinRow db; db.found = true; db.reconnect_min_ms = 9000; db.reconnect_max_ms = 500;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.reconnect_min_ms <= c.reconnect_max_ms, "clamp: reconnect min>max swapped");
    }
    {
        CabinRow db; db.found = true; db.floors_min = 30; db.floors_max = 1;
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(c.floors_min <= c.floors_max, "clamp: floors min>max -> defaults");
    }

    // ---- camera_urls parse + scheme ----
    check(parse_first_camera_url("[\"rtsp://h/s\"]") == "rtsp://h/s", "parse: rtsp url");
    check(parse_first_camera_url("[]") == "", "parse: empty array");
    check(parse_first_camera_url("") == "", "parse: empty string");
    check(is_allowed_camera_url("rtsp://h/s"), "scheme: allow rtsp");
    check(is_allowed_camera_url("http://h/s"), "scheme: allow http");
    check(!is_allowed_camera_url("videotestsrc ! appsink"), "scheme: reject pipeline");
    check(!is_allowed_camera_url(""), "scheme: reject empty");
    {
        CabinRow db; db.found = true; db.camera_urls = "[\"videotestsrc ! appsink\"]";
        CabinConfig c = resolve_cabin_config(CliOverrides{}, db, def);
        check(!c.use_stream, "resolve: DB pipeline scheme rejected -> USB fallback");
    }

    // ---- validate_cabin_patch (shared REST validation) ----
    {
        CabinPatch p; p.has_camera_urls = true; p.camera_urls = "[\"rtsp://ok/cam\"]";
        check(validate_cabin_patch(p).empty(), "patch: valid rtsp -> no errors");
    }
    {
        CabinPatch p; p.has_camera_urls = true; p.camera_urls = "[\"videotestsrc ! x\"]";
        check(!validate_cabin_patch(p).empty(), "patch: pipeline string -> rejected");
    }
    {
        CabinPatch p; p.has_match_thr = true; p.match_thr = 2.0f;
        check(!validate_cabin_patch(p).empty(), "patch: match_thr oob -> rejected");
    }
    {
        CabinPatch p; p.has_confirm_streak = true; p.confirm_streak = 0;
        check(!validate_cabin_patch(p).empty(), "patch: confirm_streak<1 -> rejected");
    }
    {
        CabinPatch p;
        p.has_reconnect_min_ms = true; p.reconnect_min_ms = 9000;
        p.has_reconnect_max_ms = true; p.reconnect_max_ms = 500;
        check(!validate_cabin_patch(p).empty(), "patch: reconnect min>max -> rejected");
    }

    printf("[test_cabin_config] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
