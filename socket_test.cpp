// socket_test.cpp — mẫu C++ ĐỌC + GHI qua socket iot_core (Socket.IO v4) trên OrangePi.
//
// Không cần thư viện ngoài: tự nói WebSocket (RFC 6455) và Engine.IO / Socket.IO v4 dạng text.
//   Đọc: 42["CONN_SUBSCRIBE",{"sn":SN,"level":LEVEL}] -> CONN_SUBSCRIBE_RESULT, rồi CONN_DATA
//        {sn, level, data}: gói đầu đầy đủ, sau đó Mode "new" chỉ chứa thanh ghi vừa đổi -> gộp.
//        Nhiều trạm (slave) cùng level dùng chung một bảng: khoá "8465-1", "8465-2"..., ADDR là
//        slave của gói vừa về; gói Mode "full" của trạm N chỉ thay các khoá "-N".
//   Ghi: 420["Client/command",{action,deviceCode,address,value,requestId}] -> ack 430[{status,mess,...}]
// SN = LEVEL = deviceCode = 2026010900100098. Khoá thanh ghi dạng "<địa chỉ>-<slave>", giá trị thô
// (nhân hệ số Cal của bảng Register), ví dụ "8465-1" là tầng hiện tại.
//
// Biên dịch: g++ -std=c++17 -O2 -o socket_test socket_test.cpp
// Chạy:      ./socket_test                       chỉ đọc trong 20 giây
//            ./socket_test 8193-1 2              đọc + ghi remotefloor = 2 (gọi tầng 2)
//            HOST=orangepi.local ./socket_test   từ máy khác cùng mạng
// Biến môi trường: HOST (127.0.0.1), PORT (3000), SN, LEVEL, DURATION (giây, 20).

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <string>

namespace {

int g_fd = -1;
std::string g_rx;  // byte đã nhận nhưng chưa tách thành frame
std::mt19937 g_rng{std::random_device{}()};

std::string env(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

std::string now() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

// ─── TCP ────────────────────────────────────────────────────────────────
int connectTcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    addrinfo* res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;

    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool sendAll(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(g_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Nhận thêm byte vào g_rx, chờ tối đa timeoutMs. Trả false khi kết nối đã đóng.
bool readMore(int timeoutMs) {
    pollfd pfd{g_fd, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0) return true;
    char buf[8192];
    ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    g_rx.append(buf, static_cast<size_t>(n));
    return true;
}

std::string base64(const unsigned char* data, size_t len) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < len ? table[(n >> 6) & 63] : '=';
        out += i + 2 < len ? table[n & 63] : '=';
    }
    return out;
}

// ─── WebSocket (RFC 6455) ───────────────────────────────────────────────
bool wsHandshake(const std::string& host, const std::string& port) {
    unsigned char key[16];
    for (auto& b : key) b = static_cast<unsigned char>(g_rng() & 0xFF);

    const std::string request =
        "GET /socket.io/?EIO=4&transport=websocket HTTP/1.1\r\n"
        "Host: " + host + ":" + port + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + base64(key, sizeof(key)) + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!sendAll(request)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_rx.find("\r\n\r\n") == std::string::npos) {
        if (!readMore(500) || std::chrono::steady_clock::now() > deadline) return false;
    }
    const size_t end = g_rx.find("\r\n\r\n");
    const std::string head = g_rx.substr(0, end);
    g_rx.erase(0, end + 4);
    return head.rfind("HTTP/1.1 101", 0) == 0;
}

// Gửi một frame; frame từ client BẮT BUỘC che mặt nạ.
bool wsSend(const std::string& payload, uint8_t opcode = 0x1) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    const uint64_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(0x80 | len);
    } else if (len < 65536) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame += static_cast<char>((len >> (8 * i)) & 0xFF);
    }
    unsigned char mask[4];
    for (auto& b : mask) b = static_cast<unsigned char>(g_rng() & 0xFF);
    frame.append(reinterpret_cast<char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) frame += static_cast<char>(payload[i] ^ mask[i % 4]);
    return sendAll(frame);
}

// Tách một message text hoàn chỉnh khỏi g_rx (gộp frame phân mảnh, tự trả lời ping WebSocket).
// Trả 1 khi có message, 0 khi chưa đủ byte, -1 khi server đóng.
int wsNext(std::string& out) {
    static std::string partial;
    while (g_rx.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(g_rx[0]);
        const auto b1 = static_cast<unsigned char>(g_rx[1]);
        const bool fin = b0 & 0x80;
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        uint64_t len = b1 & 0x7F;
        size_t pos = 2;

        if (len == 126) {
            if (g_rx.size() < 4) return 0;
            len = (static_cast<unsigned char>(g_rx[2]) << 8) | static_cast<unsigned char>(g_rx[3]);
            pos = 4;
        } else if (len == 127) {
            if (g_rx.size() < 10) return 0;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(g_rx[2 + i]);
            pos = 10;
        }
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (g_rx.size() < pos + 4) return 0;
            std::memcpy(mask, g_rx.data() + pos, 4);
            pos += 4;
        }
        if (g_rx.size() < pos + len) return 0;

        std::string payload = g_rx.substr(pos, len);
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        g_rx.erase(0, pos + len);

        if (opcode == 0x8) return -1;                        // close
        if (opcode == 0x9) { wsSend(payload, 0xA); continue; }  // ping -> pong
        if (opcode == 0xA) continue;                         // pong
        partial += payload;                                  // text (0x1) hoặc phần nối (0x0)
        if (!fin) continue;
        out.swap(partial);
        partial.clear();
        return 1;
    }
    return 0;
}

// ─── JSON phẳng tối giản (đủ cho gói CONN_DATA) ────────────────────────
// Gói full của trạm `slave`: bỏ các khoá thanh ghi "-<slave>" trước khi gộp, giữ trạm khác.
// Gói không có ADDR thì thay cả bảng.
void dropSlaveRegisters(std::map<std::string, std::string>& reading, const std::string& slave) {
    if (slave.empty() || slave == "-") {
        reading.clear();
        return;
    }
    const std::regex slaveKey("a?\\d+-" + slave);
    for (auto it = reading.begin(); it != reading.end();) {
        if (std::regex_match(it->first, slaveKey)) it = reading.erase(it);
        else ++it;
    }
}

// Vị trí '{' của object đứng sau "key": trong chuỗi s, hoặc npos.
size_t findObject(const std::string& s, const std::string& key) {
    const size_t k = s.find("\"" + key + "\":");
    if (k == std::string::npos) return std::string::npos;
    const size_t brace = s.find('{', k);
    return brace;
}

// Vị trí '}' đóng object bắt đầu ở s[pos]=='{' (bỏ qua ngoặc nằm trong chuỗi), hoặc npos.
size_t objectEnd(const std::string& s, size_t pos) {
    int depth = 0;
    bool inString = false;
    for (size_t i = pos; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

// Đọc object phẳng {"k":v,...} bắt đầu ở s[pos]=='{' thành map key -> giá trị (chuỗi bỏ nháy).
std::map<std::string, std::string> parseFlatObject(const std::string& s, size_t pos) {
    std::map<std::string, std::string> out;
    size_t i = pos + 1;
    auto readString = [&](size_t& j) {
        std::string value;
        for (++j; j < s.size() && s[j] != '"'; ++j) {
            if (s[j] == '\\' && j + 1 < s.size()) ++j;
            value += s[j];
        }
        ++j;  // bỏ dấu nháy đóng
        return value;
    };
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
        if (i >= s.size() || s[i] == '}') break;
        if (s[i] != '"') break;
        const std::string key = readString(i);
        while (i < s.size() && (s[i] == ' ' || s[i] == ':')) ++i;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            value = readString(i);
        } else {
            int depth = 0;  // bỏ qua object/mảng lồng nếu có
            for (; i < s.size(); ++i) {
                if (s[i] == '{' || s[i] == '[') ++depth;
                if (s[i] == '}' || s[i] == ']') { if (depth == 0) break; --depth; }
                if (s[i] == ',' && depth == 0) break;
                value += s[i];
            }
        }
        out[key] = value;
    }
    return out;
}

std::string get(const std::map<std::string, std::string>& m, const std::string& key) {
    auto it = m.find(key);
    return it == m.end() ? "-" : it->second;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = env("HOST", "127.0.0.1");
    const std::string port = env("PORT", "3000");
    const std::string sn = env("SN", "2026010900100098");
    const std::string level = env("LEVEL", "2026010900100098");
    const int duration = std::stoi(env("DURATION", "20"));
    const bool doWrite = argc >= 3;
    const std::string address = doWrite ? argv[1] : "";
    const std::string value = doWrite ? argv[2] : "";

    if (doWrite && !std::regex_match(address, std::regex("\\d+-\\d+"))) {
        std::cerr << "address phai dang <thanh ghi>-<slave>, vi du 8193-1\n";
        return 2;
    }
    if (doWrite && !std::regex_match(value, std::regex("-?\\d+"))) {
        std::cerr << "value phai la so nguyen\n";
        return 2;
    }

    g_fd = connectTcp(host, port);
    if (g_fd < 0 || !wsHandshake(host, port)) {
        std::cerr << now() << " khong noi duoc iot_core " << host << ":" << port << "\n";
        return 1;
    }
    std::cout << now() << " da noi iot_core " << host << ":" << port << "\n";

    std::map<std::string, std::string> reading;  // bản đầy đủ đang giữ
    bool subscribed = false;
    bool written = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);

    while (std::chrono::steady_clock::now() < deadline) {
        if (!readMore(200)) {
            std::cerr << now() << " iot_core dong ket noi\n";
            break;
        }
        std::string msg;
        int r;
        while ((r = wsNext(msg)) == 1) {
            if (msg[0] == '0') {                       // Engine.IO open -> mở namespace "/"
                wsSend("40");
            } else if (msg == "2") {                   // Engine.IO ping -> pong
                wsSend("3");
            } else if (msg.rfind("40", 0) == 0 && !subscribed) {   // đã vào namespace -> đăng ký
                wsSend("42[\"CONN_SUBSCRIBE\",{\"sn\":\"" + sn + "\",\"level\":\"" + level + "\"}]");
                subscribed = true;
            } else if (msg.rfind("42[\"CONN_SUBSCRIBE_RESULT\"", 0) == 0) {
                std::cout << now() << " subscribe: " << msg.substr(2) << "\n";
            } else if (msg.rfind("42[\"CONN_DATA\"", 0) == 0) {
                const size_t pos = findObject(msg, "data");
                if (pos == std::string::npos) continue;
                const auto data = parseFlatObject(msg, pos);
                if (get(data, "Mode") == "full") dropSlaveRegisters(reading, get(data, "ADDR"));
                // reading = bảng đầy đủ đang giữ, ví dụ reading["8465-1"] là tầng hiện tại.
                for (const auto& [key, val] : data) reading[key] = val;

                // In nguyên JSON của gói vừa nhận: gói đầu đầy đủ, các gói sau chỉ phần đổi.
                const size_t end = objectEnd(msg, pos);
                if (end != std::string::npos) std::cout << msg.substr(pos, end - pos + 1) << "\n";

                if (doWrite && !written) {             // có dữ liệu rồi mới ghi
                    wsSend("420[\"Client/command\",{\"action\":\"write\",\"deviceCode\":\"" + sn +
                           "\",\"address\":\"" + address + "\",\"value\":" + value +
                           ",\"requestId\":\"cpp-test\"}]");
                    std::cout << now() << " gui Client/command ghi " << address << " = " << value << "\n";
                    written = true;
                }
            } else if (msg.rfind("430", 0) == 0) {     // ack của lệnh ghi (id 0)
                std::cout << now() << " ack ghi: " << msg.substr(3) << "\n";
            }
        }
        if (r < 0) {
            std::cerr << now() << " iot_core dong WebSocket\n";
            break;
        }
    }

    wsSend("", 0x8);  // đóng WebSocket lịch sự
    close(g_fd);
    return 0;
}
