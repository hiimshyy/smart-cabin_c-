#include "socketio_client.h"

#include "log/logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::uint64_t steady_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string json_escape_string(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

bool socketio_ack_id(const std::string& message, int& ack_id) {
    if (message.size() < 4 || message[0] != '4' || message[1] != '3') return false;
    size_t pos = 2;
    long long value = 0;
    bool has_digit = false;
    while (pos < message.size() && std::isdigit(static_cast<unsigned char>(message[pos]))) {
        has_digit = true;
        value = value * 10 + (message[pos] - '0');
        if (value > std::numeric_limits<int>::max()) return false;
        ++pos;
    }
    if (!has_digit || pos >= message.size() || message[pos] != '[') return false;
    ack_id = static_cast<int>(value);
    return true;
}

bool json_string_field(const std::string& json, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) return false;
    size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '\"') return false;
    ++pos;
    out.clear();
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) { out += c; escaped = false; }
        else if (c == '\\') escaped = true;
        else if (c == '\"') return true;
        else out += c;
    }
    return false;
}

bool json_bool_field(const std::string& json, const char* key, bool& out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) return false;
    size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

std::string base64(const unsigned char* data, size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < n) x |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < n) x |= data[i + 2];
        out += kB64[(x >> 18) & 63]; out += kB64[(x >> 12) & 63];
        out += i + 1 < n ? kB64[(x >> 6) & 63] : '=';
        out += i + 2 < n ? kB64[x & 63] : '=';
    }
    return out;
}

// Small self-contained SHA-1 for the RFC 6455 Sec-WebSocket-Accept check.
std::array<unsigned char, 20> sha1(const std::string& input) {
    std::vector<unsigned char> data(input.begin(), input.end());
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    uint32_t h0=0x67452301U,h1=0xEFCDAB89U,h2=0x98BADCFEU,h3=0x10325476U,h4=0xC3D2E1F0U;
    for (size_t off = 0; off < data.size(); off += 64) {
        uint32_t w[80]{};
        for (int i=0;i<16;++i) w[i]=(uint32_t(data[off+i*4])<<24)|(uint32_t(data[off+i*4+1])<<16)|
                                      (uint32_t(data[off+i*4+2])<<8)|data[off+i*4+3];
        for (int i=16;i<80;++i) { uint32_t x=w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=(x<<1)|(x>>31); }
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i=0;i<80;++i) {
            uint32_t f,k;
            if (i<20) { f=(b&c)|((~b)&d); k=0x5A827999U; }
            else if (i<40) { f=b^c^d; k=0x6ED9EBA1U; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCU; }
            else { f=b^c^d; k=0xCA62C1D6U; }
            uint32_t r=(a<<5)|(a>>27); uint32_t t=r+f+e+k+w[i]; e=d; d=c; c=(b<<30)|(b>>2); b=a; a=t;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    std::array<unsigned char,20> out{};
    const uint32_t hs[5]={h0,h1,h2,h3,h4};
    for(int i=0;i<5;++i) for(int j=0;j<4;++j) out[i*4+j]=static_cast<unsigned char>(hs[i]>>(24-j*8));
    return out;
}

bool send_all(int fd, const std::string& data, int timeout_ms) {
    size_t off=0;
    while (off < data.size()) {
        const ssize_t n=::send(fd,data.data()+off,data.size()-off,MSG_NOSIGNAL);
        if(n>0){off+=static_cast<size_t>(n);continue;}
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)) {
            pollfd p{fd,POLLOUT,0}; if(::poll(&p,1,timeout_ms)>0) continue;
        }
        return false;
    }
    return true;
}

int connect_tcp(const WsEndpoint& ep, int timeout_ms) {
    addrinfo hints{}; hints.ai_socktype=SOCK_STREAM; hints.ai_family=AF_UNSPEC;
    addrinfo* result=nullptr;
    if(::getaddrinfo(ep.host.c_str(),ep.port.c_str(),&hints,&result)!=0) return -1;
    int fd=-1;
    for(addrinfo* p=result;p;p=p->ai_next) {
        fd=::socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(fd<0) continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(fd); fd = -1; continue;
        }
        if(::connect(fd,p->ai_addr,p->ai_addrlen)!=0 && errno!=EINPROGRESS) {
            ::close(fd); fd=-1; continue;
        }
        pollfd ready{fd, POLLOUT, 0};
        int so_error = 0; socklen_t so_len = sizeof(so_error);
        if (::poll(&ready, 1, timeout_ms) <= 0 ||
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0 ||
            ::fcntl(fd, F_SETFL, flags) != 0) {
            ::close(fd); fd = -1; continue;
        }
        break;
    }
    ::freeaddrinfo(result); return fd;
}

int read_more(int fd, std::string& bytes, int timeout_ms) {
    pollfd p{fd,POLLIN,0}; const int r=::poll(&p,1,timeout_ms);
    if(r==0) return 0;
    if(r<0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) return -1;
    char buf[4096]; const ssize_t n=::recv(fd,buf,sizeof(buf),0);
    if(n<=0) return -1;
    bytes.append(buf,static_cast<size_t>(n));
    return 1;
}

bool has_token_ci(const std::string& value, const char* token) {
    std::string a=value,b=token;
    for(char& c:a)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for(char& c:b)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b)!=std::string::npos;
}

bool handshake(int fd, const WsEndpoint& ep, int timeout_ms, std::string& rx) {
    std::array<unsigned char,16> nonce{}; std::random_device rd;
    for(auto& b:nonce)b=static_cast<unsigned char>(rd());
    const std::string key=base64(nonce.data(),nonce.size());
    const std::string req="GET "+ep.target+" HTTP/1.1\r\nHost: "+ep.host+":"+ep.port+
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "+key+
        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if(!send_all(fd,req,timeout_ms)) return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    while(rx.find("\r\n\r\n")==std::string::npos) {
        const auto now=std::chrono::steady_clock::now(); if(now>=deadline) return false;
        const int remain=static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count());
        if(read_more(fd,rx,std::max(1,remain)) < 0) return false;
    }
    const size_t end=rx.find("\r\n\r\n"); const std::string header=rx.substr(0,end); rx.erase(0,end+4);
    if(header.rfind("HTTP/1.1 101",0)!=0) return false;
    const std::string want=websocket_accept_for_key(key);
    bool upgrade=false, connection=false, accept=false;
    std::istringstream lines(header); std::string line; std::getline(lines,line);
    while(std::getline(lines,line)) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        const size_t colon=line.find(':');
        if(colon==std::string::npos) continue;
        std::string name=line.substr(0,colon), value=line.substr(colon+1); while(!value.empty()&&value.front()==' ')value.erase(0,1);
        for(char& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(name=="upgrade")upgrade=has_token_ci(value,"websocket");
        else if(name=="connection")connection=has_token_ci(value,"upgrade");
        else if(name=="sec-websocket-accept")accept=(value==want);
    }
    return upgrade&&connection&&accept;
}

bool next_server_text(std::string& rx, std::string& fragments, std::string& out, size_t max_payload,
                      int fd, int timeout_ms) {
    while(rx.size()>=2) {
        const unsigned char b0=static_cast<unsigned char>(rx[0]), b1=static_cast<unsigned char>(rx[1]);
        const bool fin=(b0&0x80)!=0; const unsigned char opcode=b0&0x0f; const bool masked=(b1&0x80)!=0;
        uint64_t len=b1&0x7f; size_t pos=2;
        if(masked) return false;  // servers must not mask frames.
        if(len==126){if(rx.size()<4)return true;len=(uint64_t(static_cast<unsigned char>(rx[2]))<<8)|static_cast<unsigned char>(rx[3]);pos=4;}
        else if(len==127){if(rx.size()<10)return true; if(static_cast<unsigned char>(rx[2])&0x80)return false;len=0;for(int i=0;i<8;++i)len=(len<<8)|static_cast<unsigned char>(rx[2+i]);pos=10;}
        if(len>max_payload || rx.size()<pos+len)return len<=max_payload;
        std::string payload=rx.substr(pos,static_cast<size_t>(len)); rx.erase(0,pos+static_cast<size_t>(len));
        if(opcode==0x8) return false;
        if(opcode==0x9){std::array<unsigned char,4> m{};std::random_device rd;for(auto& x:m)x=static_cast<unsigned char>(rd());if(!send_all(fd,websocket_client_frame(payload,0xA,m.data()),timeout_ms))return false;continue;}
        if(opcode==0xA)continue;
        if(opcode!=0x1&&opcode!=0x0)return false;
        if(fragments.size()+payload.size()>max_payload) return false;
        fragments+=payload;
        if(fin){out.swap(fragments);fragments.clear();return true;}
    }
    return true;
}
}  // namespace

bool parse_ws_endpoint(const std::string& url, WsEndpoint& out) {
    constexpr const char kPrefix[]="ws://";
    if(url.rfind(kPrefix,0)!=0) return false;
    const std::string rest=url.substr(sizeof(kPrefix)-1); const size_t slash=rest.find('/');
    const std::string authority=rest.substr(0,slash); if(authority.empty()||authority.find('@')!=std::string::npos) return false;
    out.host=authority; out.port="80"; out.target=slash==std::string::npos?"/socket.io/?EIO=4&transport=websocket":rest.substr(slash);
    const size_t colon=authority.rfind(':');
    if(colon!=std::string::npos){out.host=authority.substr(0,colon);out.port=authority.substr(colon+1);}
    if(out.host.empty()||out.port.empty()||out.target.empty()||out.target[0]!='/')return false;
    unsigned long port=0; for(char c:out.port){if(c<'0'||c>'9')return false;port=port*10+unsigned(c-'0');if(port>65535)return false;}
    return port>0;
}

std::string websocket_accept_for_key(const std::string& key) {
    const auto digest=sha1(key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); return base64(digest.data(),digest.size());
}

std::string websocket_client_text_frame(const std::string& text, const unsigned char mask[4]) {
    return websocket_client_frame(text, 0x1, mask);
}

std::string websocket_client_frame(const std::string& text, unsigned char opcode, const unsigned char mask[4]) {
    std::string out; out.push_back(static_cast<char>(0x80 | (opcode & 0x0f))); const uint64_t n=text.size();
    if(n<126)out.push_back(static_cast<char>(0x80|n)); else if(n<=65535){out.push_back(static_cast<char>(0xfe));out.push_back(static_cast<char>(n>>8));out.push_back(static_cast<char>(n));}
    else {out.push_back(static_cast<char>(0xff));for(int i=7;i>=0;--i)out.push_back(static_cast<char>(n>>(i*8)));}
    out.append(reinterpret_cast<const char*>(mask),4); for(size_t i=0;i<text.size();++i)out.push_back(static_cast<char>(text[i]^mask[i%4])); return out;
}

bool floor_to_register_value(int floor, std::int64_t& value) {
    // home_floor <= 0 is the project's "no floor" sentinel. Limit the shift to
    // a positive signed 64-bit JSON integer so there is no undefined overflow.
    if (floor <= 0 || floor > 62) return false;
    value = std::int64_t{1} << floor;
    return true;
}

std::string socketio_floor_command_packet(int ack_id, const SocketIoConfig& config,
                                          std::int64_t value) {
    return socketio_floor_command_packet(ack_id, config, value, config.request_id);
}

std::string socketio_floor_command_packet(int ack_id, const SocketIoConfig& config,
                                          std::int64_t value,
                                          const std::string& request_id) {
    return "42" + std::to_string(ack_id) +
        "[\"Client/command\",{\"action\":\"write\",\"deviceCode\":\"" +
        json_escape_string(config.device_code) + "\",\"address\":\"" +
        json_escape_string(config.floor_address) + "\",\"value\":" +
        std::to_string(value) + ",\"requestId\":\"" +
        json_escape_string(request_id) + "\"}]";
}

bool parse_socketio_command_ack(const std::string& message, SocketIoCommandResult& out) {
    int ack_id = -1;
    if (!socketio_ack_id(message, ack_id)) return false;
    const size_t body = message.find('[', 2);
    const size_t object_begin = message.find('{', body);
    const size_t object_end = message.rfind('}');
    if (body == std::string::npos || object_begin == std::string::npos ||
        object_end == std::string::npos || object_end <= object_begin) return false;
    const std::string json = message.substr(object_begin, object_end - object_begin + 1);
    std::string request_id;
    bool success = false;
    if (!json_string_field(json, "requestId", request_id) ||
        !json_bool_field(json, "success", success)) return false;
    out = SocketIoCommandResult{};
    out.socket_ack_id = ack_id;
    out.request_id = request_id;
    json_string_field(json, "code", out.code);
    json_string_field(json, "message", out.message);
    out.state = success
        ? (out.code == "ALREADY_SELECTED" ? SocketIoCommandState::AckedDuplicate
                                             : SocketIoCommandState::AckedSuccess)
        : SocketIoCommandState::AckedError;
    return true;
}

SocketIoClient::SocketIoClient(SocketIoConfig config):config_(std::move(config)){}
SocketIoClient::~SocketIoClient(){close();}
bool SocketIoClient::start(){
    WsEndpoint ep;
    if(!parse_ws_endpoint(config_.endpoint,ep) || config_.device_code.empty() ||
       config_.floor_address.empty() || config_.request_id.empty() || config_.max_queue == 0 ||
       config_.ack_timeout_ms <= 0 || config_.command_max_age_ms <= 0) return false;
    if(worker_.joinable())return true;
    stop_=false;worker_=std::thread(&SocketIoClient::worker_loop,this);return true;
}
void SocketIoClient::close(){stop_=true;if(worker_.joinable())worker_.join();connected_=false;}

void SocketIoClient::set_status_callback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_callback_ = std::move(callback);
}

void SocketIoClient::report(SocketIoCommandResult result) {
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = status_callback_;
    }
    if (callback) callback(result);
}

bool SocketIoClient::send_floor(int floor) {
    std::int64_t value = 0;
    if (!floor_to_register_value(floor, value)) {
        LOG_ERROR("socketio", "floor cannot be encoded as register bitmask floor=%d", floor);
        return false;
    }
    return enqueue_floor(SocketIoFloorCommand{config_.request_id, floor, value});
}

bool SocketIoClient::enqueue_floor(const SocketIoFloorCommand& command) {
    if (command.request_id.empty() || command.floor <= 0 || command.value <= 0) return false;
    if (stop_ || !worker_.joinable()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= config_.max_queue) {
        ++dropped_;
        return false;
    }
    queue_.push_back(Command{command.request_id, command.floor, command.value, steady_ms()});
    return true;
}

void SocketIoClient::worker_loop() {
    WsEndpoint ep; parse_ws_endpoint(config_.endpoint,ep); int backoff=config_.reconnect_min_ms;
    int next_ack_id = 0;
    while(!stop_) {
        int fd=connect_tcp(ep,config_.connect_timeout_ms); std::string rx;
        if(fd<0||!handshake(fd,ep,config_.connect_timeout_ms,rx)) {
            if(fd>=0) ::close(fd);
            LOG_WARN("socketio","connect/handshake failed; retrying host=%s port=%s",
                     ep.host.c_str(), ep.port.c_str());
        } else {
            LOG_INFO("socketio","WebSocket connected; awaiting Engine.IO/Socket.IO session");
            bool alive=true, namespace_sent=false, namespace_ready=false;
            bool in_flight=false;
            int in_flight_ack=-1;
            Command in_flight_command;
            std::uint64_t sent_at_ms=0;
            std::string fragments;
            while(alive&&!stop_) {
                std::string msg;
                if(!next_server_text(rx,fragments,msg,config_.max_payload,fd,config_.connect_timeout_ms)){alive=false;break;}
                if(!msg.empty()) {
                    if(msg[0]=='0'&&!namespace_sent){std::array<unsigned char,4> m{};std::random_device rd;for(auto& x:m)x=static_cast<unsigned char>(rd());alive=send_all(fd,websocket_client_text_frame("40",m.data()),config_.connect_timeout_ms);namespace_sent=true;}
                    else if(msg=="2"){std::array<unsigned char,4> m{};std::random_device rd;for(auto& x:m)x=static_cast<unsigned char>(rd());alive=send_all(fd,websocket_client_text_frame("3",m.data()),config_.connect_timeout_ms);}
                    else if(msg.rfind("40",0)==0){namespace_ready=true;connected_=true;LOG_INFO("socketio","Socket.IO namespace '/' connected");}
                    else {
                        int ack_id = -1;
                        if (socketio_ack_id(msg, ack_id) && in_flight && ack_id == in_flight_ack) {
                            SocketIoCommandResult result;
                            if (!parse_socketio_command_ack(msg, result) ||
                                result.request_id != in_flight_command.request_id) {
                                result = SocketIoCommandResult{in_flight_command.request_id,
                                                               in_flight_command.floor, ack_id,
                                                               SocketIoCommandState::AckedError,
                                                               "MALFORMED_ACK",
                                                               "ACK missing or mismatched requestId/success"};
                            } else {
                                result.floor = in_flight_command.floor;
                            }
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (!queue_.empty()) queue_.pop_front();
                            }
                            if (result.state == SocketIoCommandState::AckedSuccess ||
                                result.state == SocketIoCommandState::AckedDuplicate) ++acked_;
                            else ++failed_;
                            in_flight = false;
                            LOG_INFO("socketio", "floor command ack received ack_id=%d request_id=%s code=%s",
                                     ack_id, result.request_id.c_str(), result.code.c_str());
                            report(std::move(result));
                        }
                    }
                }

                if (namespace_ready && !in_flight) {
                    Command next;
                    std::vector<Command> expired;
                    const std::uint64_t now = steady_ms();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        while (!queue_.empty() && now - queue_.front().enqueued_ms >
                               static_cast<std::uint64_t>(config_.command_max_age_ms)) {
                            expired.push_back(queue_.front());
                            queue_.pop_front();
                            ++dropped_;
                            LOG_WARN("socketio", "expired floor command dropped before send");
                        }
                        if (!queue_.empty()) {
                            next = queue_.front();
                        }
                    }
                    for (const Command& expired_command : expired) {
                        report(SocketIoCommandResult{expired_command.request_id, expired_command.floor, -1,
                                                     SocketIoCommandState::Expired, "EXPIRED",
                                                     "command expired before send"});
                    }
                    if (next.value > 0) {
                        const int ack_id = next_ack_id;
                        next_ack_id = next_ack_id == std::numeric_limits<int>::max() ? 0 : next_ack_id + 1;
                        std::array<unsigned char,4> mask{}; std::random_device rd;
                        for(auto& x:mask)x=static_cast<unsigned char>(rd());
                        const std::string packet = socketio_floor_command_packet(
                            ack_id, config_, next.value, next.request_id);
                        alive = send_all(fd, websocket_client_text_frame(packet, mask.data()),
                                         config_.connect_timeout_ms);
                        if (alive) {
                            ++sent_;
                            in_flight = true;
                            in_flight_ack = ack_id;
                            in_flight_command = next;
                            sent_at_ms = now;
                            LOG_INFO("socketio", "TX %s", packet.c_str());
                            LOG_INFO("socketio", "floor command sent floor=%d value=%lld ack_id=%d",
                                     next.floor, (long long)next.value, ack_id);
                            report(SocketIoCommandResult{next.request_id, next.floor, ack_id,
                                                         SocketIoCommandState::Sent, "", ""});
                        }
                    }
                }

                if (in_flight && steady_ms() - sent_at_ms >=
                                 static_cast<std::uint64_t>(config_.ack_timeout_ms)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!queue_.empty()) queue_.pop_front();
                    }
                    ++failed_;
                    in_flight = false;
                    LOG_ERROR("socketio", "floor command ack timeout ack_id=%d", in_flight_ack);
                    report(SocketIoCommandResult{in_flight_command.request_id, in_flight_command.floor,
                                                 in_flight_ack, SocketIoCommandState::Ambiguous,
                                                 "ACK_TIMEOUT", "command may have reached IoT Core"});
                }
                if(!alive||stop_)break;
                if(read_more(fd,rx,250) < 0) alive=false;
            }
            if (in_flight) {
                // The write may already have reached the service. Do not blindly resend an
                // ambiguous command after reconnect because requestId is fixed to "ai-core".
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) queue_.pop_front();
                ++failed_;
                LOG_ERROR("socketio", "connection lost before floor command ack");
                report(SocketIoCommandResult{in_flight_command.request_id, in_flight_command.floor,
                                             in_flight_ack, SocketIoCommandState::Ambiguous,
                                             "CONNECTION_LOST", "command may have reached IoT Core"});
            }
            if(connected_.exchange(false)) LOG_WARN("socketio","Socket.IO session disconnected");
            ::close(fd);
            backoff=config_.reconnect_min_ms;
        }
        for(int slept=0;slept<backoff&&!stop_;slept+=50)std::this_thread::sleep_for(std::chrono::milliseconds(50));
        backoff=std::min(backoff*2,config_.reconnect_max_ms);
    }
}
