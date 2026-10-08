#include "socketio_client.h"

#include <cstdio>
#include <cstring>
#include <string>

static int checks = 0, failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s\n", m); } } while (0)

int main() {
    WsEndpoint endpoint;
    CHECK(parse_ws_endpoint("ws://127.0.0.1:3000/socket.io/?EIO=4&transport=websocket", endpoint),
          "parse provisional endpoint");
    CHECK(endpoint.host == "127.0.0.1" && endpoint.port == "3000" &&
          endpoint.target == "/socket.io/?EIO=4&transport=websocket", "parsed endpoint fields");
    CHECK(!parse_ws_endpoint("wss://service/socket.io/", endpoint), "wss rejected until TLS is implemented");
    CHECK(!parse_ws_endpoint("http://service/", endpoint), "http rejected");
    CHECK(websocket_accept_for_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
          "RFC 6455 Sec-WebSocket-Accept example");

    const unsigned char mask[4] = {1, 2, 3, 4};
    const std::string frame = websocket_client_text_frame("40", mask);
    CHECK(frame.size() == 8 && static_cast<unsigned char>(frame[0]) == 0x81 &&
          static_cast<unsigned char>(frame[1]) == 0x82, "masked client text frame header");
    CHECK(static_cast<unsigned char>(frame[6]) == (static_cast<unsigned char>('4') ^ 1) &&
          static_cast<unsigned char>(frame[7]) == (static_cast<unsigned char>('0') ^ 2), "text payload is masked");
    const std::string pong = websocket_client_frame("x", 0xA, mask);
    CHECK(static_cast<unsigned char>(pong[0]) == 0x8A, "WebSocket pong opcode");

    SocketIoConfig config;
    std::int64_t value = 0;
    CHECK(floor_to_register_value(5, value) && value == 32,
          "floor 5 maps to register value 2^5 = 32");
    CHECK(!floor_to_register_value(0, value), "unset floor 0 is rejected");
    CHECK(!floor_to_register_value(63, value), "floor outside signed 64-bit range is rejected");
    floor_to_register_value(5, value);
    const std::string command = socketio_floor_command_packet(0, config, value);
    CHECK(command ==
          "420[\"Client/command\",{\"action\":\"write\","
          "\"deviceCode\":\"2026010900100098\",\"address\":\"8193-2\","
          "\"value\":32,\"requestId\":\"ai-core\"}]",
          "first floor command matches IoT Core contract exactly");
    CHECK(socketio_floor_command_packet(17, config, 4).rfind("4217[", 0) == 0,
          "Socket.IO ack id is encoded after event packet type");
    CHECK(socketio_floor_command_packet(2, config, 32, "ai-core:2:7:1").find(
              "\"requestId\":\"ai-core:2:7:1\"") != std::string::npos,
          "command accepts a unique business request id");
    SocketIoCommandResult ack;
    CHECK(parse_socketio_command_ack(
              "430[{\"requestId\":\"ai-core:2:7:1\",\"success\":true,\"code\":\"OK\"}]", ack) &&
              ack.request_id == "ai-core:2:7:1" && ack.state == SocketIoCommandState::AckedSuccess,
          "success ACK parses request id and state");
    CHECK(parse_socketio_command_ack(
              "431[{\"requestId\":\"ai-core:2:7:1\",\"success\":true,\"code\":\"ALREADY_SELECTED\"}]", ack) &&
              ack.state == SocketIoCommandState::AckedDuplicate,
          "already-selected ACK is a successful duplicate");
    CHECK(parse_socketio_command_ack(
              "432[{\"requestId\":\"ai-core:2:7:1\",\"success\":false,\"code\":\"FORBIDDEN_MODE\"}]", ack) &&
              ack.state == SocketIoCommandState::AckedError,
          "negative ACK is an error");
    CHECK(!parse_socketio_command_ack("433[]", ack), "ACK without contract payload is rejected");
    std::printf("[test_socketio_client] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
