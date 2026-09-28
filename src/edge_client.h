#pragma once
// UDS client to Edge Core (spec edge-core-uds, đợt a: chiều AI->Edge).
//
// AI Core = client; Edge Core = server on an AF_UNIX/SOCK_STREAM socket. All
// socket I/O runs on ONE background thread (mirror ResidentDB::writer_loop) so
// the realtime frame loop never blocks. Framing = 8-byte header + JSON body.
// No external dependency (POSIX sockets + std C++17 + edge_json).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// ---- Framing constants + message types (R2, §3) ---------------------------
static constexpr uint8_t MAGIC0     = 0xAA;
static constexpr uint8_t MAGIC1     = 0x55;
static constexpr size_t  HEADER_LEN = 8;

enum class MsgType : uint16_t {
    EVENT_FACE_RECOG_DETECTED = 0x1001,
    EVENT_FACE_UNKNOWN        = 0x1002,
    CMD_HEARTBEAT_PING        = 0x1003,
    // (đợt b) RESP_CALL_FLOOR_RESULT = 0x2001, CMD_FACE_REGISTER_REQUEST = 0x2002
};

// Encode one framed message: 8-byte header (magic + htons(type) + htonl(len))
// followed by body, all in a single std::string for one-shot write_all.
std::string encode_frame(MsgType t, const std::string& body);

// Write exactly n bytes to fd, looping over partial writes and retrying EINTR.
// Uses MSG_NOSIGNAL so a dead peer yields EPIPE instead of SIGSTOP-ing us.
// Returns false on any write error (caller closes fd + reconnects).
bool write_all(int fd, const char* p, size_t n);

// ---- EdgeClient (R4, R5, R6) ----------------------------------------------
struct EdgeConfig {
    std::string socket_path;          // "" => disabled; "@..." => abstract namespace
    int    cabin_id         = 1;
    int    reconnect_min_ms = 500;
    int    reconnect_max_ms = 10000;
    int    heartbeat_ms     = 5000;
    int    send_timeout_ms  = 500;    // SO_SNDTIMEO so a hung Edge Core can't wedge us
    size_t max_queue        = 256;    // bounded; drop-oldest when exceeded (R4.3)
};

class EdgeClient {
public:
    explicit EdgeClient(const EdgeConfig& cfg);
    ~EdgeClient();                     // calls close()

    EdgeClient(const EdgeClient&)            = delete;
    EdgeClient& operator=(const EdgeClient&) = delete;

    bool start();                      // spawn sender thread; false if socket_path empty
    void close();                      // flush best-effort + join (R4.4)

    // Fire-and-forget (called from frame thread) — only enqueue, return now.
    void send_recog(const std::string& ext_or_id, const std::string& name,
                    int home_floor, float similarity, int64_t ts_ms);
    void send_unknown(float confidence, int64_t ts_ms);

    // Diagnostics (for tests + logging).
    uint64_t sent()    const { return sent_; }
    uint64_t dropped() const { return dropped_; }

private:
    struct Job { MsgType type; std::string body; };
    void worker_loop();
    bool connect_locked();             // create fd + connect (path or abstract)
    void enqueue(MsgType t, std::string body);   // drop-oldest if full

    EdgeConfig cfg_;
    int        fd_ = -1;
    std::thread worker_;
    std::mutex  mtx_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool        stop_ = false;
    uint64_t    sent_    = 0;
    uint64_t    dropped_ = 0;
};
