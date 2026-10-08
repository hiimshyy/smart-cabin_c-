#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Socket.IO v4 client used by AI Core to write the recognized resident's
// home_floor to the elevator service without blocking the inference thread.
struct SocketIoConfig {
    std::string endpoint;  // ws://host:port/socket.io/?EIO=4&transport=websocket
    std::string device_code = "2026010900100098";
    std::string floor_address = "8193-2";
    std::string request_id = "ai-core";
    int reconnect_min_ms = 500;
    int reconnect_max_ms = 10000;
    int connect_timeout_ms = 5000;
    int ack_timeout_ms = 3000;
    int command_max_age_ms = 5000;
    size_t max_payload = 1024 * 1024;
    size_t max_queue = 64;
};

struct WsEndpoint {
    std::string host;
    std::string port;
    std::string target;
};

bool parse_ws_endpoint(const std::string& url, WsEndpoint& out);
std::string websocket_accept_for_key(const std::string& key);
std::string websocket_client_text_frame(const std::string& text,
                                        const unsigned char mask[4]);
std::string websocket_client_frame(const std::string& payload, unsigned char opcode,
                                   const unsigned char mask[4]);
bool floor_to_register_value(int floor, std::int64_t& value);
std::string socketio_floor_command_packet(int ack_id, const SocketIoConfig& config,
                                          std::int64_t value);
std::string socketio_floor_command_packet(int ack_id, const SocketIoConfig& config,
                                          std::int64_t value,
                                          const std::string& request_id);

enum class SocketIoCommandState : std::uint8_t {
    Sent,
    AckedSuccess,
    AckedDuplicate,
    AckedError,
    Expired,
    Ambiguous,
};

struct SocketIoFloorCommand {
    std::string request_id;
    int floor = 0;
    std::int64_t value = 0;
};

struct SocketIoCommandResult {
    std::string request_id;
    int floor = 0;
    int socket_ack_id = -1;
    SocketIoCommandState state = SocketIoCommandState::AckedError;
    std::string code;
    std::string message;
};

// Parses a Socket.IO acknowledgement packet such as
// 431[{"requestId":"ai-core:1:1:1","success":true,"code":"OK"}].
// The command acknowledgement contract requires requestId and success.
bool parse_socketio_command_ack(const std::string& message, SocketIoCommandResult& out);

class SocketIoClient {
public:
    using StatusCallback = std::function<void(const SocketIoCommandResult&)>;
    explicit SocketIoClient(SocketIoConfig config);
    ~SocketIoClient();
    SocketIoClient(const SocketIoClient&) = delete;
    SocketIoClient& operator=(const SocketIoClient&) = delete;

    bool start();
    void close();
    // Non-blocking. Register value = 2^floor; floor 5 produces "value":32.
    // Legacy helper: uses SocketIoConfig::request_id.
    bool send_floor(int floor);
    bool enqueue_floor(const SocketIoFloorCommand& command);
    void set_status_callback(StatusCallback callback);
    bool connected() const { return connected_.load(); }
    uint64_t sent() const { return sent_.load(); }
    uint64_t acked() const { return acked_.load(); }
    uint64_t failed() const { return failed_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

private:
    struct Command {
        std::string request_id;
        int floor = 0;
        std::int64_t value = 0;
        std::uint64_t enqueued_ms = 0;
    };
    void report(SocketIoCommandResult result);
    void worker_loop();
    SocketIoConfig config_;
    std::thread worker_;
    std::mutex mutex_;
    std::deque<Command> queue_;
    std::mutex callback_mutex_;
    StatusCallback status_callback_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> acked_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<uint64_t> dropped_{0};
};
