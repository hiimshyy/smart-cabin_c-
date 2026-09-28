// Unit tests for edge_json (spec edge-core-uds, Task 6.2). g++ + -Isrc only.
#include "edge_json.h"
#include <cstdio>
#include <string>

static int g_checks = 0, g_fail = 0;
static void check(bool ok, const char* msg) {
    ++g_checks; if (!ok) { ++g_fail; printf("  FAIL: %s\n", msg); }
}
static bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

int main() {
    // ---- json_escape ----
    check(json_escape("a\"b") == "a\\\"b", "escape double-quote");
    check(json_escape("a\\b") == "a\\\\b", "escape backslash");
    check(json_escape("a\nb") == "a\\nb",  "escape newline");
    check(json_escape("a\tb") == "a\\tb",  "escape tab");
    check(json_escape(std::string("a\x01""b")) == "a\\u0001b", "escape control -> \\u00XX");
    // Vietnamese UTF-8 kept verbatim (byte-identical).
    check(json_escape("Lê Nhật Tân") == "Lê Nhật Tân", "UTF-8 Vietnamese verbatim");

    // ---- JsonObj kvf locale-independent %.2f ----
    {
        JsonObj o; o.kvf("k", 0.415);
        check(o.str() == "{\"k\":0.41}" || o.str() == "{\"k\":0.42}", "kvf %.2f");
        check(!contains(o.str(), ","), "kvf uses '.' decimal (no comma)");
    }
    // ---- kvi int64 boundaries ----
    {
        JsonObj o; o.kvi("n", -9223372036854775807LL);
        check(contains(o.str(), "-9223372036854775807"), "kvi large negative int64");
    }

    // ---- 0x1001 recog body ----
    {
        std::string b = build_recog_body(1, "0842", "Lê Nhật Tân", 6, 0.87f, 1773909000000LL);
        check(contains(b, "\"20000\":1773909000000"), "recog 20000 ts");
        check(contains(b, "\"20001\":1"),             "recog 20001 cabin");
        check(contains(b, "\"20002\":\"0842\""),      "recog 20002 id");
        check(contains(b, "\"20003\":\"Lê Nhật Tân\""), "recog 20003 name");
        check(contains(b, "\"20005\":6"),             "recog 20005 floor");
        check(contains(b, "\"20008\":0.87"),          "recog 20008 similarity %.2f");
        check(!contains(b, "20007"),                  "recog has NO 20007");
    }

    // ---- 0x1002 unknown body (custom format: 20003=UNKNOWN_FACE + 20008) ----
    {
        std::string b = build_unknown_body(1, 0.41f, 1773909005000LL);
        check(contains(b, "\"20000\":1773909005000"),   "unknown 20000 ts");
        check(contains(b, "\"20001\":1"),               "unknown 20001 cabin");
        check(contains(b, "\"20003\":\"UNKNOWN_FACE\""),"unknown 20003=UNKNOWN_FACE");
        check(contains(b, "\"20008\":0.41"),            "unknown 20008 confidence %.2f");
        check(!contains(b, "status"),                   "unknown has NO 'status' key");
        check(!contains(b, "confidence\""),             "unknown has NO 'confidence' key");
    }

    // ---- 0x1003 heartbeat body ----
    {
        std::string b = build_heartbeat_body(1773909000000LL);
        check(contains(b, "\"20000\":1773909000000"), "heartbeat 20000 ts");
    }

    printf("[test_edge_json] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
