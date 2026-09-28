// Unit tests for edge framing + transport (spec edge-core-uds, Task 6.3).
// encode_frame byte layout + round-trip over socketpair (anti-glue-packet).
// g++ + -Isrc, -lpthread; no NPU/OpenCV.
#include "edge_client.h"
#include "edge_json.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

static int g_checks = 0, g_fail = 0;
static void check(bool ok, const char* msg) {
    ++g_checks; if (!ok) { ++g_fail; printf("  FAIL: %s\n", msg); }
}

// Read exactly n bytes (blocking) — test helper mirroring an Edge Core reader.
static bool read_exact(int fd, char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = ::read(fd, p + off, n - off);
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

int main() {
    // ---- encode_frame byte layout ----
    {
        std::string body = "{\"20000\":1}";
        std::string f = encode_frame(MsgType::EVENT_FACE_RECOG_DETECTED, body);
        check(f.size() == HEADER_LEN + body.size(), "frame len = 8 + body");
        check((unsigned char)f[0] == 0xAA, "magic0 0xAA");
        check((unsigned char)f[1] == 0x55, "magic1 0x55");
        uint16_t nt; std::memcpy(&nt, &f[2], 2);
        check(ntohs(nt) == 0x1001, "msg_type ntohs == 0x1001");
        uint32_t nlen; std::memcpy(&nlen, &f[4], 4);
        check(ntohl(nlen) == body.size(), "payload_len ntohl == body.size()");
        check(std::string(f.data() + 8, body.size()) == body, "body preserved");
    }

    // ---- Round-trip over socketpair: TWO frames back-to-back must split
    //      cleanly on the reader side (anti glue-packet). ----
    {
        int sv[2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair created");

        std::string b1 = build_heartbeat_body(111);
        std::string b2 = build_unknown_body(2, 0.5f, 222);
        std::string f1 = encode_frame(MsgType::CMD_HEARTBEAT_PING, b1);
        std::string f2 = encode_frame(MsgType::EVENT_FACE_UNKNOWN, b2);

        // Writer side: write both frames in one contiguous buffer (glued).
        std::string both = f1 + f2;
        check(write_all(sv[0], both.data(), both.size()), "write_all two frames");

        // Reader side: parse frame-by-frame using the length prefix.
        auto read_frame = [&](int fd, uint16_t& type, std::string& body) -> bool {
            char hdr[HEADER_LEN];
            if (!read_exact(fd, hdr, HEADER_LEN)) return false;
            if ((unsigned char)hdr[0] != 0xAA || (unsigned char)hdr[1] != 0x55) return false;
            uint16_t nt; std::memcpy(&nt, &hdr[2], 2); type = ntohs(nt);
            uint32_t nlen; std::memcpy(&nlen, &hdr[4], 4);
            uint32_t len = ntohl(nlen);
            body.resize(len);
            if (len && !read_exact(fd, &body[0], len)) return false;
            return true;
        };

        uint16_t t1 = 0, t2 = 0; std::string r1, r2;
        check(read_frame(sv[1], t1, r1), "read frame 1");
        check(t1 == 0x1003 && r1 == b1, "frame 1 type+body match (heartbeat)");
        check(read_frame(sv[1], t2, r2), "read frame 2");
        check(t2 == 0x1002 && r2 == b2, "frame 2 type+body match (unknown)");

        ::close(sv[0]); ::close(sv[1]);
    }

    // ---- Reconnect resilience + bounded-queue drop-oldest (Task 6.4) ----
    // Point at a non-existent socket: connect must fail without crashing, and
    // pumping more than max_queue jobs must drop the oldest (dropped_ > 0).
    {
        EdgeConfig ec;
        ec.socket_path     = "/tmp/edge_nonexistent_test.sock";
        ec.reconnect_min_ms = 50;
        ec.reconnect_max_ms = 100;
        ec.max_queue        = 8;
        EdgeClient c(ec);
        check(c.start(), "EdgeClient.start() with bad path returns true (thread up)");
        // Pump way more than max_queue; worker never connects so nothing drains.
        for (int i = 0; i < 100; ++i) c.send_unknown(0.1f * i, 1000 + i);
        // Give the worker a moment (it should be stuck in connect backoff, not draining).
        usleep(200 * 1000);
        c.close();   // must not hang
        check(c.dropped() > 0, "bounded queue dropped oldest (dropped_ > 0)");
        check(c.sent() == 0,   "nothing sent (never connected)");
    }

    printf("[test_edge_frame] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
