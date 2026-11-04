# Design — AI Core ↔ Edge Core qua Unix Domain Socket (edge-core-uds)

> **Legacy design — 2026-10-06:** thiết kế này khớp implementation UDS đang chạy nhưng đã bị thay
> thế cho hướng phát triển mới bởi Socket.IO v4. Không tái sử dụng header 8 byte hay tag `0x100x`
> bên trong WebSocket. Thiết kế migration và các quyết định còn thiếu nằm tại
> [websocket-migration-review.md](websocket-migration-review.md). Từ 2026-10-08, AI Core trực tiếp
> gửi `Client/command` gọi tầng sau confirmed recognition, với register `value = 2^home_floor`.

**Spec ID**: `edge-core-uds`

Tài liệu yêu cầu: [requirements.md](requirements.md)

## Overview

Thiết kế này hiện thực chiều AI Core → Edge Core (đợt a) của [requirements.md](requirements.md). AI
Core (`face_recog_app`) thêm một client UDS gửi sự kiện nhận diện sang Edge Core:

- `0x1001 EVENT_FACE_RECOG_DETECTED` khi một cư dân được confirmed.
- `0x1002 EVENT_FACE_UNKNOWN` khi phát hiện người lạ.
- `0x1003 CMD_HEARTBEAT_PING` định kỳ khi đang có kết nối.

Mỗi thông điệp gồm header 8 byte cố định và body JSON tag-số theo dải tag v2.

Quyết định chính:

- Toàn bộ I/O socket chạy trên **một thread nền**, tách khỏi vòng lặp frame realtime, mirror đúng
  pattern async `writer_loop` của `ResidentDB`. Frame thread chỉ enqueue rồi trả về.
- Hàng đợi có giới hạn, drop gói cũ nhất khi đầy (R4.3). Khi mất kết nối, sender thread reconnect
  theo backoff và giữ các job chưa gửi (R5).
- Framing và serializer JSON tự viết, không thêm dependency. `edge_json` tách khỏi socket để
  unit-test không cần fd.
- `EdgeClient` không phụ thuộc `resident_db`: `main.cpp` truyền sẵn ID, tên và tầng.
- Không truyền `--edge-socket` thì `main.cpp` không tạo `EdgeClient`, app chạy như trước (R1.4).

Ngoài phạm vi: chiều Edge→AI (đợt b, `0x2001`/`0x2002`), tag `20007`, và quyết định gọi tầng (thuộc
Edge Core). Phạm vi đầy đủ ở mục "2. Phạm vi" của requirements.

## Architecture

### Luồng dữ liệu

```
  ┌────────────────────────────── face_recog_app (AI Core) ──────────────────────────────┐
  │                                                                                       │
  │  main loop (frame thread)                    EdgeClient (sender thread)               │
  │  ─────────────────────────                   ───────────────────────────              │
  │  interaction.update(...) → Outcomes                                                   │
  │        │                                     ┌───────────────────────────┐            │
  │        ├─ confirmed ─▶ edge.send_recog(...) ─┤  bounded queue (mutex+CV)  │            │
  │        ├─ unknown   ─▶ edge.send_unknown(..)─┤  drop-oldest khi đầy (R4.3)│            │
  │        │                (fire-and-forget)    └────────────┬──────────────┘            │
  │        │                                                  │ dequeue                   │
  │  (heartbeat timer) ────▶ enqueue 0x1003 ─────────────────▶│                           │
  │                                                           ▼                           │
  │                                          ┌──────────────────────────────────┐         │
  │                                          │ encode_frame(): header8 + JSON    │         │
  │                                          │ write_all() tới fd                │         │
  │                                          │ connect/reconnect + backoff (R5)  │         │
  │                                          └────────────────┬─────────────────┘         │
  └───────────────────────────────────────────────────────────┼─────────────────────────┘
                                                               │ AF_UNIX / SOCK_STREAM
                                                               ▼
                                          /run/elev_edge_core.sock  (Edge Core = SERVER)
                                          hoặc @elev_edge_core.sock (abstract namespace)
```

**Nguyên tắc**: người gọi ở frame thread chỉ `enqueue` rồi trả về ngay. Mọi `connect`/`send`/
`reconnect` nằm gọn trong sender thread. Frame thread không bao giờ chạm fd socket.

### Chiều phụ thuộc

- `edge_json` không phụ thuộc gì (chỉ std).
- `edge_client` phụ thuộc `edge_json` + POSIX socket + logger.
- `main.cpp` phụ thuộc `edge_client` + đọc `Resident.ext_id`.
- `edge_client` KHÔNG phụ thuộc `resident_db` — main truyền sẵn các giá trị (id string, name, floor)
  vào `send_recog`, nên module socket độc lập, dễ test.

### Vòng đời trong `main.cpp`

1. Khởi động: chỉ ở nhánh `use_resident_db`, sau khi mở DB thành công và khi có `--edge-socket`,
   `main.cpp` tạo `EdgeClient` rồi gọi `start()`.
2. Mỗi frame: outcome `confirmed` gọi `send_recog`, outcome `unknown` gọi `send_unknown`.
3. Shutdown: `edge->close()` cùng chỗ dọn dẹp các resource khác.

Chi tiết móc nối ở mục "Tích hợp vào `main.cpp`" bên dưới.

## Components and Interfaces

### Bản đồ file

| File | Thay đổi |
|---|---|
| `src/edge_client.h` (mới) | Khai báo `EdgeClient`, `EdgeConfig`, struct payload (`RecogEvent`, `UnknownEvent`); hằng `msg_type`, `MAGIC0/MAGIC1`, `HEADER_LEN=8` |
| `src/edge_client.cpp` (mới) | UDS connect/reconnect, framing encode, `write_all`, sender `worker_loop`, bounded queue, heartbeat |
| `src/edge_json.h` / `src/edge_json.cpp` (mới) | Serializer JSON tối giản (escape string, build object tag-số). Tách khỏi socket để unit-test không cần fd |
| `src/app_config.h` | Thêm `const char* edge_socket = nullptr;` + (không cần cờ Passed vì không đọc DB) |
| `src/app_config.cpp` | Parse `--edge-socket <path>`; thêm dòng usage |
| `src/resident_db.h` | `struct Resident` thêm `std::string ext_id;` |
| `src/resident_db.cpp` | `load_active`: SELECT thêm cột `ext_id`, gán vào `Resident.ext_id` |
| `src/main.cpp` | Khởi tạo `EdgeClient` khi có `--edge-socket` (chỉ ở nhánh `use_resident_db`); trong outcome loop gọi `edge.send_recog(...)` / `edge.send_unknown(...)`; `edge.close()` lúc shutdown |
| `Makefile` | Thêm `edge_client.cpp` + `edge_json.cpp` vào `DB_SRCS` (link vào `face_recog_app`). Không lib mới |
| `tests/test_edge_json.cpp` (mới) | Unit test serialize + escape |
| `tests/test_edge_frame.cpp` (mới) | Unit test encode header (magic, byte order, payload_len); round-trip qua `socketpair()` |
| `tests/run_tests.sh` | Thêm 2 stanza biên dịch + chạy test mới |

### Framing: `encode_frame` và `write_all` (R2)

Layout header 8 byte và hằng `MsgType` nằm ở mục Data Models.

`encode_frame(MsgType t, const std::string& body) -> std::string`:

```cpp
std::string encode_frame(MsgType t, const std::string& body) {
    std::string out;
    out.resize(HEADER_LEN + body.size());
    out[0] = (char)MAGIC0;
    out[1] = (char)MAGIC1;
    uint16_t nt  = htons((uint16_t)t);
    uint32_t nlen = htonl((uint32_t)body.size());
    std::memcpy(&out[2], &nt,  2);
    std::memcpy(&out[4], &nlen, 4);
    std::memcpy(&out[8], body.data(), body.size());
    return out;   // gửi one-shot bằng write_all()
}
```

Gộp header + body vào **một buffer** rồi `write_all` một lần → không có nguy cơ header đi mà body
kẹt giữa chừng ở tầng app. `write_all` vẫn phải lặp cho tới hết vì `send()` có thể ghi một phần
(R2.4).

```cpp
// Trả false nếu lỗi ghi (caller sẽ đóng fd + reconnect).
bool write_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = ::send(fd, p + off, n - off, MSG_NOSIGNAL); // MSG_NOSIGNAL: EPIPE thay vì SIGPIPE
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && (errno == EINTR)) continue;
        return false;   // EPIPE/ECONNRESET/... -> reconnect
    }
    return true;
}
```

### Serializer JSON `edge_json` (R3)

Tự viết (tiền lệ `parse_first_camera_url`). Không thư viện. Chỉ cần object phẳng, value string
hoặc số.

```cpp
// edge_json.h — builder tối giản, không cấp phát ngoài std::string.
std::string json_escape(const std::string& s);          // ", \, và control < 0x20

class JsonObj {
    std::string buf_;   // nội dung giữa {}
    void sep() { if (!buf_.empty()) buf_ += ','; }
public:
    JsonObj& kv (const char* k, const std::string& v) { sep(); buf_ += '"'; buf_ += k; buf_ += "\":\""; buf_ += json_escape(v); buf_ += '"'; return *this; }
    JsonObj& kvi(const char* k, int64_t v)            { sep(); buf_ += '"'; buf_ += k; buf_ += "\":";   buf_ += std::to_string(v);  return *this; }
    JsonObj& kvf(const char* k, double v);             // format cố định vài chữ số, locale-independent
    std::string str() const { std::string o = "{"; o += buf_; o += "}"; return o; }
};
```

**Escape (R3.2)**: `"`→`\"`, `\`→`\\`, `\b \f \n \r \t`, còn lại `< 0x20` → `\u00XX`. Byte UTF-8 ≥
0x80 **giữ nguyên** (tên tiếng Việt "Lê Nhật Tân" đi thẳng, JSON cho phép UTF-8 thô trong string).

**`kvf` (R3.4 confidence)**: format không phụ thuộc locale (tránh dấu `,` thập phân theo locale VN).
Dùng `snprintf(buf, "%.2f", v)`. Điều kiện để đúng: xem Property 6 và mục "Quyết định mở và rủi ro".

Ba hàm `build_recog_body`, `build_unknown_body`, `build_heartbeat_body` trong `edge_json` dùng
`JsonObj` để dựng body cho từng `msg_type` (định dạng ở mục Data Models).

### `EdgeClient`: vòng đời và thread nền (R4, R5, R6)

Cấu hình `EdgeConfig` nằm ở mục Data Models.

```cpp
// edge_client.h
class EdgeClient {
public:
    explicit EdgeClient(const EdgeConfig& cfg);
    ~EdgeClient();                     // gọi close()

    bool start();                      // spawn sender thread; false nếu socket_path rỗng
    void close();                      // flush best-effort + join (R4.4)

    // Fire-and-forget (gọi từ frame thread) — chỉ enqueue rồi return.
    void send_recog(const std::string& ext_or_id, const std::string& name,
                    int home_floor, float similarity, int64_t ts_ms);
    void send_unknown(float confidence, int64_t ts_ms);

private:
    struct Job { MsgType type; std::string body; };
    void  worker_loop();               // connect/reconnect + dequeue + write
    bool  connect_locked();            // tạo fd, connect (path hoặc abstract)
    void  enqueue(MsgType t, std::string body);   // đẩy vào queue, drop-oldest nếu đầy

    EdgeConfig cfg_;
    int        fd_ = -1;
    std::thread worker_;
    std::mutex  mtx_;
    std::condition_variable cv_;
    std::deque<Job> q_;                // bounded bởi cfg_.max_queue
    bool        stop_ = false;
    uint64_t    sent_ = 0, dropped_ = 0;
};
```

**`worker_loop` (mirror `ResidentDB::writer_loop`)**:

1. Nếu `fd_ < 0`: thử `connect_locked()`. Thất bại → sleep theo backoff (min→max, có jitter), log
   tiết chế (R5.3), quay lại.
2. Chờ trên `cv_` cho tới khi có job HOẶC hết `heartbeat_ms` (timed wait) HOẶC `stop_`.
3. Timeout heartbeat mà đang connected → enqueue nội bộ một `0x1003` (R6).
4. Drain queue: mỗi job `write_all(fd_, frame)`. Nếu `write_all` false → đóng fd (`fd_=-1`), giữ
   nguyên các job còn lại trong queue để gửi sau khi reconnect (R5.2), quay lại bước 1.
5. `stop_` → flush best-effort (cố gửi nốt trong thời gian ngắn) rồi thoát.

**Backoff**: bắt đầu `reconnect_min_ms`, nhân đôi tới `reconnect_max_ms`, reset về min sau khi
connect thành công. Jitter nhẹ để tránh đồng bộ hoá.

**Bounded queue (R4.3)**: `enqueue` giữ khóa; nếu `q_.size() >= max_queue` thì `q_.pop_front()`
(drop gói cũ nhất) + `++dropped_`, rồi mới `push_back`. Log đếm định kỳ, không mỗi lần.

**Connect abstract vs pathname (R1.2)**:

```cpp
struct sockaddr_un addr{};
addr.sun_family = AF_UNIX;
const std::string& p = cfg_.socket_path;
if (!p.empty() && p[0] == '@') {                 // abstract namespace
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, p.data() + 1, p.size() - 1);
    socklen_t len = offsetof(sockaddr_un, sun_path) + p.size();  // dùng độ dài thực
    ::connect(fd, (sockaddr*)&addr, len);
} else {                                          // pathname
    std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
    ::connect(fd, (sockaddr*)&addr, sizeof(addr));
}
```

**Chống treo (NFR)**: set `SO_SNDTIMEO` trên fd để một Edge Core treo (nhận nửa chừng rồi đứng)
không khóa vĩnh viễn thread nền; timeout → coi như lỗi ghi → reconnect.

### Tích hợp vào `main.cpp` (R7, R8)

Khai báo gần các object operational (main.cpp ~ dòng 200, cạnh `InteractionManager interaction;`):

```cpp
std::unique_ptr<EdgeClient> edge;   // nullptr nếu không có --edge-socket
```

Khởi tạo trong nhánh `use_resident_db` sau khi mở DB thành công:

```cpp
if (cfg.edge_socket && cfg.edge_socket[0]) {
    EdgeConfig ec;
    ec.socket_path     = cfg.edge_socket;
    ec.cabin_id        = cfg.cabin_id;
    ec.reconnect_min_ms= eff.reconnect_min_ms;   // tái dùng backoff của CabinConfig
    ec.reconnect_max_ms= eff.reconnect_max_ms;
    edge = std::make_unique<EdgeClient>(ec);
    if (!edge->start()) { LOG_WARN("edge", "EdgeClient start failed; disabled"); edge.reset(); }
    else LOG_INFO("edge", "EdgeClient → %s", cfg.edge_socket);
}
```

Trong outcome loop (main.cpp ~619-644):

```cpp
if (oc.confirmed) {
    // ... log_event + touch_resident như cũ ...
    if (edge) {
        auto it = resident_by_id.find(oc.resident_id);
        std::string id_str = (it != resident_by_id.end() && !it->second.ext_id.empty())
                             ? it->second.ext_id
                             : std::to_string(oc.resident_id);           // R7.3 fallback
        std::string name   = (it != resident_by_id.end()) ? it->second.name : "";
        int         floor  = (it != resident_by_id.end()) ? it->second.home_floor : 0;
        edge->send_recog(id_str, name, floor, oc.similarity, (int64_t)now_ms()); // R7.1 (+20008)
    }
} else if (oc.unknown) {
    // ... log_event như cũ ...
    if (edge) edge->send_unknown(oc.similarity, (int64_t)now_ms());      // R3.4
}
```

Shutdown: `if (edge) edge->close();` cùng chỗ dọn dẹp các resource khác trước khi return.

> **Lưu ý PII (NFR)**: `name` chỉ đi vào body JSON gửi Edge Core (hợp lệ về nghiệp vụ). Các dòng
> `LOG_*` của module vẫn chỉ in `resident_id`/đếm gói, không in tên.

## Data Models

### Header khung 8 byte (R2)

Header 8 byte cố định, sau đó là body JSON:

```
 offset  size  field         nội dung
 ──────  ────  ───────────   ─────────────────────────────────────────────
   0      1    magic[0]      0xAA
   1      1    magic[1]      0x55
   2      2    msg_type      uint16, network byte order (htons)
   4      4    payload_len   uint32, network byte order (htonl) = strlen(body)
   8      N    body          JSON UTF-8, N == payload_len byte
```

```cpp
// edge_client.h
static constexpr uint8_t  MAGIC0     = 0xAA;
static constexpr uint8_t  MAGIC1     = 0x55;
static constexpr size_t   HEADER_LEN = 8;

enum class MsgType : uint16_t {
    EVENT_FACE_RECOG_DETECTED = 0x1001,
    EVENT_FACE_UNKNOWN        = 0x1002,
    CMD_HEARTBEAT_PING        = 0x1003,
    // (đợt b) RESP_CALL_FLOOR_RESULT = 0x2001, CMD_FACE_REGISTER_REQUEST = 0x2002
};
```

### `EdgeConfig`

```cpp
// edge_client.h
struct EdgeConfig {
    std::string socket_path;          // "" => tắt; "@..." => abstract namespace
    int    cabin_id           = 1;
    int    reconnect_min_ms   = 500;
    int    reconnect_max_ms   = 10000;
    int    heartbeat_ms       = 5000;
    size_t max_queue          = 256;  // bounded; drop-oldest khi vượt (R4.3)
};
```

`main.cpp` điền `socket_path` từ `--edge-socket`, `cabin_id` từ `AppConfig.cabin_id`, và hai giá trị
backoff từ `CabinConfig` hiệu lực. `heartbeat_ms` và `max_queue` giữ default.

### `Resident.ext_id` (R8.3)

`struct Resident` thêm `std::string ext_id;`. `ResidentDB::load_active` SELECT thêm cột
`residents.ext_id` và gán vào field này, nên lúc phát sự kiện không phải truy vấn DB trong vòng lặp
nóng. Spec này không thêm migration schema.

### Dải tag v2

Spec dùng **dải tag v2** (đã chốt): `cabin_id` = `20001`, các tag v1 từ `20001` trở đi dời +1. Xem
bảng đối chiếu đầy đủ ở `requirements.md` §4. Tóm tắt các tag AI Core dùng ở chiều gửi:

| Tag | Ý nghĩa | Kiểu |
|---|---|---|
| `20000` | timestamp ms | int64 |
| `20001` | cabin_id | int |
| `20002` | mã NV / ID | string |
| `20003` | họ tên | string |
| `20005` | tầng mặc định | int |
| `20008` | độ tương đồng nhận diện (0..1) | float |

### Payload theo từng msg_type

`0x1001` (nhận diện được — AI→Edge):

```json
{"20000":1773909000000,"20001":1,"20002":"0842","20003":"Lê Nhật Tân","20005":6,"20008":0.87}
```

> Không có `20007` (R7.4). `20002` = ext_id, fallback `to_string(resident_id)` (R7.3).
> `20001` = cabin_id từ `AppConfig.cabin_id` (R7.2). `20008` = `Outcome.similarity`, format `%.2f`.

`0x1002` (người lạ — AI→Edge):

```json
{"20000":1773909005000,"20001":1,"20003":"UNKNOWN_FACE","20008":0.41}
```

> Tag số nhất quán với `0x1001` (không dùng key chữ). `20003` = literal `"UNKNOWN_FACE"` (ô "họ tên"
> mang trạng thái người lạ, theo quy ước chốt với Edge Core). `20008` = similarity tốt nhất của
> phiên (nếu có), else `0`, format `%.2f`. `20001` = cabin_id.

`0x1003` heartbeat:

```json
{"20000":1773909000000}
```

**`cabin_id` (R7.2)**: chốt **đưa vào payload dưới dạng tag số `20001`** (dải v2), Edge Core parse
như mọi tag khác — không cần key chữ riêng. Đây là điểm đã đồng bộ liên-team (dải tag v2).

## *Correctness Properties

*Property là đặc tính phải đúng trên mọi lần chạy hợp lệ của hệ thống. Mỗi property nối một yêu cầu
trong [requirements.md](requirements.md) với cách kiểm chứng ở mục Testing Strategy.*

Trong dòng **Validates**, `Rx.y` là yêu cầu `Rx` ý thứ `y` ở mục "5. Yêu cầu chức năng" của
requirements.md; "NFR" là mục "6. Yêu cầu phi chức năng".

### *Property 1: Header khung đúng layout

*Với mọi* `MsgType` và body, `encode_frame` trả chuỗi dài đúng `8 + body.size()` byte. Byte 0–1 là
`0xAA 0x55`, `ntohs` của byte 2–3 bằng `msg_type`, `ntohl` của byte 4–7 bằng số **byte** của body
(không phải số ký tự UTF-8), và byte 8 trở đi trùng body.

**Validates: Requirements R2.1, R2.2, R2.3**

### *Property 2: Ghi đủ và tách gói đúng

*Với mọi* dãy frame được ghi liên tiếp bằng `write_all` lên một `SOCK_STREAM`, bên nhận đọc 8 byte
header rồi đúng `payload_len` byte body sẽ thu lại đúng dãy frame ban đầu, không dính hay vỡ gói.
`write_all` chỉ trả `true` khi đã ghi hết `n` byte; partial write và `EINTR` được lặp tiếp.

**Validates: Requirements R2.4**

### *Property 3: Escape JSON giữ nguyên UTF-8

*Với mọi* chuỗi đầu vào, `json_escape` thay `"` và `\` bằng dạng escape, thay mọi byte điều khiển
`< 0x20` bằng `\b \f \n \r \t` hoặc `\u00XX`, và giữ nguyên từng byte `≥ 0x80`. Kết quả đặt trong
cặp `"` luôn là JSON string hợp lệ.

**Validates: Requirements R3.1, R3.2**

### *Property 4: Payload `0x1001` đúng tập tag

*Với mọi* sự kiện confirmed, body `0x1001` chứa đúng các tag `20000` (int64), `20001` (cabin_id từ
`EdgeConfig.cabin_id`), `20002` (string), `20003` (string), `20005` (int) và `20008` (float `%.2f`)
theo thứ tự đó, và không chứa `20007`. `20002` bằng `Resident.ext_id` khi khác rỗng, ngược lại bằng
`std::to_string(resident_id)`.

**Validates: Requirements R3.3, R7.1, R7.2, R7.3, R7.4**

### *Property 5: Payload `0x1002` và `0x1003` chỉ dùng tag số

*Với mọi* sự kiện unknown, body `0x1002` chứa `20000`, `20001`, `20003 = "UNKNOWN_FACE"` và `20008`
(similarity tốt nhất của phiên, hoặc `0`), không có key chữ như `status` hay `confidence`. Body
heartbeat `0x1003` chứa `20000`.

**Validates: Requirements R3.4, R3.5, R7.2**

### *Property 6: Số thực dùng dấu `.` thập phân

*Với mọi* giá trị `double`, khi `LC_NUMERIC` của tiến trình là locale `C`, `kvf` in đúng 2 chữ số
thập phân với dấu `.`, nên body vẫn là JSON hợp lệ.

**Validates: Requirements R3.3, R3.4**

### *Property 7: Frame thread không chạm socket

*Với mọi* lời gọi `send_recog`/`send_unknown` từ frame thread, hàm chỉ dựng body, lấy `mtx_`, đẩy job
vào `q_`, notify `cv_` rồi trả về. Hàm không gọi `connect`/`send` và không dùng `fd_`. Heartbeat cũng
đi qua `q_`, không có đường ghi socket thứ hai.

**Validates: Requirements R4.1, R4.2, R6.2, NFR không chặn realtime**

### *Property 8: Hàng đợi có giới hạn, drop job cũ nhất

*Với mọi* chuỗi lời gọi `send_*`, sau mỗi `enqueue` thì `q_.size() ≤ max_queue`. Khi hàng đợi đầy,
job bị bỏ là job cũ nhất, `dropped_` tăng 1 cho mỗi job bị bỏ, và job mới luôn được nhận. Điều này
đúng cả khi đang mất kết nối.

**Validates: Requirements R4.3, R5.2**

### *Property 9: Lỗi socket dẫn tới reconnect

*Với mọi* lỗi connect (`ENOENT`, `ECONNREFUSED`) hoặc lỗi ghi (`EPIPE`, `ECONNRESET`, hết hạn
`SO_SNDTIMEO`), sender thread đóng fd (`fd_ = -1`), giữ các job chưa gửi trong `q_`, và thử connect
lại sau khoảng chờ bắt đầu từ `reconnect_min_ms`, nhân đôi tới `reconnect_max_ms` (có jitter nhẹ),
reset về min sau khi connect thành công. App và sender thread không crash, và không có `SIGPIPE`.

**Validates: Requirements R1.3, R4.5, R5.1, R5.2**

### *Property 10: Heartbeat chỉ khi có kết nối

*Với mọi* khoảng `heartbeat_ms` không có job trong lúc đang connected, sender thread enqueue một
`0x1003`. Khi mất kết nối, sender thread không sinh heartbeat.

**Validates: Requirements R6.1, R6.3**

### *Property 11: Địa chỉ pathname và abstract namespace

*Với mọi* `socket_path` bắt đầu bằng `@`, `sun_path[0] = '\0'`, phần sau `@` được copy vào
`sun_path + 1`, và độ dài địa chỉ truyền cho `connect` là
`offsetof(sockaddr_un, sun_path) + socket_path.size()`. Mọi `socket_path` khác được dùng như
pathname.

**Validates: Requirements R1.1, R1.2**

### *Property 12: Tắt hoàn toàn khi không cấu hình

*Với mọi* lần chạy không có `--edge-socket` (hoặc giá trị rỗng), hoặc không ở nhánh
`use_resident_db`, `main.cpp` không tạo `EdgeClient`: không thread mới, không socket. Khi `start()`
trả `false`, app log cảnh báo, bỏ `EdgeClient` và tiếp tục nhận diện.

**Validates: Requirements R1.4, R8.1, R8.4, NFR không hồi quy**

### *Property 13: Shutdown không treo

*Với mọi* trạng thái kết nối (kể cả khi Edge Core chưa từng lên), `close()` đặt `stop_`, flush
best-effort trong thời gian ngắn, join sender thread rồi trả về.

**Validates: Requirements R4.4**

### *Property 14: Log không lộ PII

*Với mọi* dòng `LOG_*` do module edge và đoạn móc trong `main.cpp` ghi, nội dung chỉ gồm trạng thái
kết nối, số đếm gói và ID số. Tên cư dân và giá trị `20002`/`20003` chỉ nằm trong body JSON gửi Edge
Core.

**Validates: Requirements NFR quyền riêng tư, hard rule về PII trong `AGENTS.md`**

## Error Handling

Nguyên tắc: lỗi ở đường gửi sang Edge Core không được làm crash app, không chặn vòng lặp frame và
không làm chết sender thread. Lỗi đi theo convention của repo (`bool` + log qua
`src/log/logger.h`), không throw. Log chỉ chứa trạng thái, số đếm và ID số.

| Tình huống | Xử lý |
|---|---|
| Không truyền `--edge-socket` hoặc giá trị rỗng | Không tạo `EdgeClient`; không thread, không socket (R1.4, R8.4) |
| `EdgeClient::start()` trả `false` | `LOG_WARN("edge", ...)`, `edge.reset()`, app tiếp tục không có Edge Core |
| Connect thất bại (`ENOENT`, `ECONNREFUSED`) | Sender thread sleep theo backoff rồi thử lại; log có tiết chế (R1.3, R5.3) |
| `send()` bị ngắt (`EINTR`) | `write_all` gọi lại `send()` |
| `send()` chỉ ghi một phần | `write_all` ghi tiếp phần còn lại (R2.4) |
| Peer đóng kết nối (`EPIPE`, `ECONNRESET`) | `MSG_NOSIGNAL` chặn `SIGPIPE`; `write_all` trả `false` → đóng fd, giữ job chưa gửi, reconnect (R4.5, R5.2) |
| Edge Core treo, không đọc | `SO_SNDTIMEO` làm `send()` hết hạn → xử lý như lỗi ghi → reconnect |
| Hàng đợi đầy (Edge Core rớt lâu) | Drop job cũ nhất, `++dropped_`, log đếm định kỳ (R4.3) |
| `send_*` được gọi khi đang mất kết nối | Vẫn enqueue tới giới hạn `max_queue` (R5.2) |
| Tới hạn heartbeat khi đang mất kết nối | Không sinh `0x1003` (R6.3) |
| `ext_id` rỗng | `20002 = std::to_string(resident_id)` (R7.3) |
| `resident_id` không có trong `resident_by_id` | `20002 = std::to_string(resident_id)`, `20003 = ""`, `20005 = 0` (theo đoạn móc trong `main.cpp`) |
| Shutdown khi còn job hoặc chưa từng kết nối | `close()` flush best-effort trong thời gian ngắn rồi join; không treo (R4.4) |

## Testing Strategy

Hai test là `main()` riêng, đã đăng ký trong `tests/run_tests.sh` và chạy bằng
`bash tests/run_tests.sh`. Cả hai build bằng `g++ -std=c++17 -Isrc`, không cần NPU/OpenCV/sqlite;
`test_edge_frame` link thêm `src/edge_client.cpp`, `src/log/logger.cpp` và `-lpthread`. Số trong
ngoặc là property mà test kiểm.

### Unit tests

**`test_edge_json.cpp`** (khối `== EdgeJson ==`, R3):

- `json_escape`: `"`, `\`, `\n`, `\t`, control `\u0001`; UTF-8 tiếng Việt giữ nguyên byte
  (Property 3).
- `kvf` format `%.2f` và dùng dấu `.` (Property 6).
- `kvi` int64 biên (số âm lớn).
- Body `0x1001` qua `build_recog_body`: có đủ `20000/20001/20002/20003/20005/20008`, `20008` format
  `%.2f`, không có `20007` (Property 4).
- Body `0x1002` qua `build_unknown_body`: `20003="UNKNOWN_FACE"` (literal), có `20001`, `20008`
  format `%.2f`, không có key `status`/`confidence` (Property 5).
- Body `0x1003` qua `build_heartbeat_body`: có `20000` (Property 5).

**`test_edge_frame.cpp`** (khối `== EdgeFrame ==`, R2 + transport qua socketpair):

- `encode_frame`: byte 0/1 = 0xAA/0x55; byte 2–3 = `ntohs` ra đúng msg_type; byte 4–7 = `ntohl` ra
  đúng `body.size()`; tổng độ dài = 8 + body (Property 1).
- Round-trip: tạo `socketpair(AF_UNIX, SOCK_STREAM)`, một đầu `write_all` hai frame liền nhau trong
  một buffer, đầu kia đọc header 8 byte + `payload_len` byte body và so khớp type + body từng frame
  (chống dính gói, Property 2).
- Reconnect + bounded queue: trỏ `socket_path` tới path không tồn tại, `max_queue = 8`, bơm 100 lần
  `send_unknown`. `start()` vẫn trả `true`, `close()` không treo, `dropped() > 0`, `sent() == 0`
  (Property 8, 9, 13). Không cần Edge Core thật.

### Build và smoke test

- `make -j4` build `face_recog_app` với `edge_client.cpp` + `edge_json.cpp` trong `DB_SRCS`.
- Chạy app không có `--edge-socket`: không thread mới, không kết nối; `run_tests.sh` pass (task 7.1,
  Property 12).

### Khoảng trống đã biết

- Partial write: chưa có test ép `send()` ghi một phần để xác nhận `write_all` lặp đủ (Property 2).
- Thứ tự tag: test chỉ kiểm từng tag có mặt trong body, không kiểm thứ tự (Property 4).
- Locale: test chạy ở locale mặc định `C`, chưa kiểm dưới locale dùng dấu `,` (Property 6).
- Abstract namespace (Property 11), `SO_SNDTIMEO` và reconnect khi peer rớt giữa chừng (Property 9),
  lịch heartbeat (Property 10), nhánh tắt trong `main.cpp` (Property 12) và log không PII
  (Property 14) chưa có unit test; cần review code hoặc test tích hợp với một Edge Core.

## Quyết định mở và rủi ro

- **`cabin_id` trong payload**: đã chốt dùng tag số `20001` (dải tag v2). `send_recog`/`send_unknown`
  KHÔNG nhận `cabin_id` làm tham số — hàm dựng body lấy từ `EdgeConfig.cabin_id` (đã cấu hình lúc
  khởi tạo `EdgeClient`), nên mọi payload tự động mang đúng cabin. Việc cập nhật file `.docx` theo
  dải v2 do đội tài liệu làm (ngoài scope code).
- **`SO_SNDTIMEO` giá trị bao nhiêu**: mặc định đề xuất ~500 ms; đủ ngắn để không kẹt frame-độc-lập
  (đằng nào cũng ở thread nền) nhưng tránh coi nhầm mạng chậm là lỗi.
- **`confidence` cho unknown**: tài liệu dùng "confidence" (độ tin cậy là mặt người), nhưng AI Core
  hiện chỉ có `similarity` (độ giống người đã biết). Đợt a gửi tạm `similarity`; nếu Edge Core cần
  face-detection score thật thì bổ sung sau (không đổi khung).
- **Locale của `kvf`**: `snprintf("%.2f")` theo `LC_NUMERIC`. `src/` không gọi `setlocale`, nên tiến
  trình ở locale `C` và in dấu `.`. Nếu sau này app hoặc một thư viện đổi `LC_NUMERIC`, `20008` có
  thể bị in dấu `,` và body không còn là JSON hợp lệ.
- **Đợt b (receiver) sẽ cần**: một read-loop tách frame (đọc 8 byte header → `payload_len` → body),
  parse `0x2001`/`0x2002`, và với `0x2002` phải trích embedding từ `20006` (đường dẫn ảnh, dải v2)
  rồi cập nhật MatchEngine/DB. Thiết kế hiện tại tách `transport` (encode/`write_all`) khỏi
  `worker_loop` nên thêm `read_frame` + dispatch là mở rộng thuận, không đập lại.
