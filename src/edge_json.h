#pragma once
// Minimal JSON serializer for Edge Core payloads (spec edge-core-uds, R3).
// Self-written, no dependency (precedent: parse_first_camera_url). Only flat
// objects with string / int64 / double values, keyed by numeric tag strings
// (dải tag v2). No NPU/OpenCV/sqlite dependency — unit-testable with g++ alone.

#include <cstdint>
#include <string>

// Escape a string for embedding inside a JSON double-quoted value:
//   "  -> \"      \  -> \\      \b \f \n \r \t
//   other control chars < 0x20 -> \u00XX
// Bytes >= 0x80 (UTF-8 multibyte, e.g. Vietnamese names) are kept verbatim.
std::string json_escape(const std::string& s);

// Flat JSON object builder. Values are appended in call order.
class JsonObj {
    std::string buf_;   // content between the outer { }
    void sep() { if (!buf_.empty()) buf_ += ','; }
public:
    JsonObj& kv(const char* k, const std::string& v) {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":\"";
        buf_ += json_escape(v); buf_ += '"';
        return *this;
    }
    JsonObj& kvi(const char* k, int64_t v) {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":";
        buf_ += std::to_string(v);
        return *this;
    }
    // Fixed 2-decimal, locale-independent (avoid VN locale decimal comma).
    JsonObj& kvf(const char* k, double v);

    std::string str() const { std::string o = "{"; o += buf_; o += "}"; return o; }
};

// ---- Body builders per msg_type (dải tag v2, spec edge-core-uds §4) --------
// 0x1001 EVENT_FACE_RECOG_DETECTED: 20000 ts, 20001 cabin, 20002 id, 20003
// name, 20005 home_floor, 20008 similarity. No 20007.
std::string build_recog_body(int cabin_id, const std::string& id,
                             const std::string& name, int home_floor,
                             float similarity, int64_t ts_ms);

// 0x1002 EVENT_FACE_UNKNOWN: 20000 ts, 20001 cabin, 20003 "UNKNOWN_FACE"
// (literal, numeric-tag convention agreed with Edge Core), 20008 confidence.
std::string build_unknown_body(int cabin_id, float confidence, int64_t ts_ms);

// 0x1003 CMD_HEARTBEAT_PING: 20000 ts.
std::string build_heartbeat_body(int64_t ts_ms);
