#include "edge_client.h"
#include "edge_json.h"
#include "log/logger.h"

#include <arpa/inet.h>      // htons, htonl
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>          // offsetof
#include <cstring>
#include <algorithm>
#include <chrono>

// ---- Framing (R2) ----------------------------------------------------------
std::string encode_frame(MsgType t, const std::string& body) {
    std::string out;
    out.resize(HEADER_LEN + body.size());
    out[0] = (char)MAGIC0;
    out[1] = (char)MAGIC1;
    uint16_t nt   = htons((uint16_t)t);
    uint32_t nlen = htonl((uint32_t)body.size());
    std::memcpy(&out[2], &nt,   2);
    std::memcpy(&out[4], &nlen, 4);
    if (!body.empty()) std::memcpy(&out[8], body.data(), body.size());
    return out;
}

bool write_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;   // EPIPE/ECONNRESET/EAGAIN(timeout)/... -> reconnect
    }
    return true;
}

// ---- EdgeClient ------------------------------------------------------------
EdgeClient::EdgeClient(const EdgeConfig& cfg) : cfg_(cfg) {}

EdgeClient::~EdgeClient() { close(); }

bool EdgeClient::start() {
    if (cfg_.socket_path.empty()) return false;   // disabled (R1.4)
    stop_ = false;
    worker_ = std::thread(&EdgeClient::worker_loop, this);
    return true;
}

void EdgeClient::close() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;   // already closed / never started
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool EdgeClient::connect_locked() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // SO_SNDTIMEO so a hung Edge Core can't wedge the sender thread forever.
    struct timeval tv;
    tv.tv_sec  = cfg_.send_timeout_ms / 1000;
    tv.tv_usec = (cfg_.send_timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string& p = cfg_.socket_path;
    socklen_t len;
    if (!p.empty() && p[0] == '@') {
        // Abstract namespace: leading NUL, then the name (without the '@').
        // Not NUL-terminated; addrlen carries the real length.
        size_t nlen = p.size() - 1;
        if (nlen > sizeof(addr.sun_path) - 1) nlen = sizeof(addr.sun_path) - 1;
        addr.sun_path[0] = '\0';
        std::memcpy(addr.sun_path + 1, p.data() + 1, nlen);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
    } else {
        std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
        len = (socklen_t)sizeof(addr);
    }

    if (::connect(fd, (struct sockaddr*)&addr, len) != 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

void EdgeClient::enqueue(MsgType t, std::string body) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        if (q_.size() >= cfg_.max_queue) {
            q_.pop_front();     // drop oldest (R4.3)
            ++dropped_;
        }
        q_.push_back(Job{t, std::move(body)});
    }
    cv_.notify_one();
}

void EdgeClient::send_recog(const std::string& id, const std::string& name,
                            int home_floor, float similarity, int64_t ts_ms) {
    enqueue(MsgType::EVENT_FACE_RECOG_DETECTED,
            build_recog_body(cfg_.cabin_id, id, name, home_floor, similarity, ts_ms));
}

void EdgeClient::send_unknown(float confidence, int64_t ts_ms) {
    enqueue(MsgType::EVENT_FACE_UNKNOWN,
            build_unknown_body(cfg_.cabin_id, confidence, ts_ms));
}

void EdgeClient::worker_loop() {
    int  backoff_ms       = cfg_.reconnect_min_ms;
    bool warned_disconnect = false;   // throttle reconnect logging (R5.3)

    while (true) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) break;
        }

        // ---- Ensure connected (R5) ----
        if (fd_ < 0) {
            if (connect_locked()) {
                LOG_INFO("edge", "connected to %s", cfg_.socket_path.c_str());
                backoff_ms        = cfg_.reconnect_min_ms;
                warned_disconnect = false;
            } else {
                if (!warned_disconnect) {
                    LOG_WARN("edge", "cannot connect to %s (retrying, backoff up to %d ms)",
                             cfg_.socket_path.c_str(), cfg_.reconnect_max_ms);
                    warned_disconnect = true;
                }
                // Sleep the backoff in slices so close() during backoff is prompt.
                int slept = 0;
                while (slept < backoff_ms) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    if (stop_) break;
                    cv_.wait_for(lk, std::chrono::milliseconds(
                        std::min(100, backoff_ms - slept)));
                    if (stop_) break;
                    slept += 100;
                }
                { std::lock_guard<std::mutex> lk(mtx_); if (stop_) break; }
                backoff_ms = std::min(backoff_ms * 2, cfg_.reconnect_max_ms);
                continue;
            }
        }

        // ---- Wait for a job or heartbeat timeout ----
        Job job;
        bool have_job = false;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.heartbeat_ms),
                         [&]{ return stop_ || !q_.empty(); });
            if (stop_) break;
            if (!q_.empty()) {
                job = std::move(q_.front());
                q_.pop_front();
                have_job = true;
            }
        }

        // Heartbeat on idle timeout while connected (R6).
        if (!have_job) {
            job = Job{MsgType::CMD_HEARTBEAT_PING, build_heartbeat_body((int64_t)0)};
            // Fill a real timestamp now (avoid stale 0).
            job.body = build_heartbeat_body(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            have_job = true;
        }

        // ---- Write it. On failure, drop fd + requeue the job for after reconnect. ----
        std::string frame = encode_frame(job.type, job.body);
        if (write_all(fd_, frame.data(), frame.size())) {
            ++sent_;
        } else {
            LOG_WARN("edge", "write failed — disconnecting (sent=%llu dropped=%llu)",
                     (unsigned long long)sent_, (unsigned long long)dropped_);
            ::close(fd_);
            fd_ = -1;
            // Requeue this job (front) so it isn't lost across reconnect (R5.2).
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!stop_) {
                    if (q_.size() >= cfg_.max_queue) { q_.pop_front(); ++dropped_; }
                    q_.push_front(std::move(job));
                }
            }
        }
    }

    // ---- Shutdown: best-effort flush of remaining jobs (R4.4) ----
    if (fd_ >= 0) {
        std::deque<Job> rest;
        { std::lock_guard<std::mutex> lk(mtx_); rest.swap(q_); }
        for (auto& j : rest) {
            std::string frame = encode_frame(j.type, j.body);
            if (!write_all(fd_, frame.data(), frame.size())) break;
            ++sent_;
        }
    }
}
