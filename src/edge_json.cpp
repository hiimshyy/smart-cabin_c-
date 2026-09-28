#include "edge_json.h"

#include <cstdio>

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Control char -> \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (int)c);
                    o += buf;
                } else {
                    // Printable ASCII or UTF-8 multibyte (>=0x80): keep verbatim.
                    o += (char)c;
                }
        }
    }
    return o;
}

JsonObj& JsonObj::kvf(const char* k, double v) {
    sep();
    buf_ += '"'; buf_ += k; buf_ += "\":";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);   // locale-independent for C locale
    buf_ += buf;
    return *this;
}

// ---- Body builders ---------------------------------------------------------
std::string build_recog_body(int cabin_id, const std::string& id,
                             const std::string& name, int home_floor,
                             float similarity, int64_t ts_ms) {
    JsonObj o;
    o.kvi("20000", ts_ms)
     .kvi("20001", cabin_id)
     .kv ("20002", id)
     .kv ("20003", name)
     .kvi("20005", home_floor)
     .kvf("20008", (double)similarity);
    return o.str();
}

std::string build_unknown_body(int cabin_id, float confidence, int64_t ts_ms) {
    JsonObj o;
    o.kvi("20000", ts_ms)
     .kvi("20001", cabin_id)
     .kv ("20003", "UNKNOWN_FACE")
     .kvf("20008", (double)confidence);
    return o.str();
}

std::string build_heartbeat_body(int64_t ts_ms) {
    JsonObj o;
    o.kvi("20000", ts_ms);
    return o.str();
}
