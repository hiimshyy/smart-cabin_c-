# Design — AI Core ↔ Edge Core qua Unix Domain Socket (edge-core-uds)

**Spec ID**: `edge-core-uds`

## 1. Kiến trúc tổng thể

AI Core (`face_recog_app`) thêm một client UDS gửi sự kiện nhận diện sang Edge Core. Toàn bộ I/O
socket chạy trên **một thread nền**, tách khỏi vòng lặp frame realtime — mirror đúng pattern async
`writer_loop` của `ResidentDB`.

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

## 2. Thay đổi & file

| File                                               | Thay đổi                                                                                                                                                                                             |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/edge_client.h` (mới)                       | Khai báo`EdgeClient`, `EdgeConfig`, struct payload (`RecogEvent`, `UnknownEvent`); hằng `msg_type`, `MAGIC0/MAGIC1`, `HEADER_LEN=8`                                                    |
| `src/edge_client.cpp` (mới)                     | UDS connect/reconnect, framing encode,`write_all`, sender `worker_loop`, bounded queue, heartbeat                                                                                                  |
| `src/edge_json.h` / `src/edge_json.cpp` (mới) | Serializer JSON tối giản (escape string, build object tag-số). Tách khỏi socket để unit-test không cần fd                                                                                     |
| `src/app_config.h`                               | Thêm`const char* edge_socket = nullptr;` + (không cần cờ Passed vì không đọc DB)                                                                                                             |
| `src/app_config.cpp`                             | Parse`--edge-socket <path>`; thêm dòng usage                                                                                                                                                       |
| `src/resident_db.h`                              | `struct Resident` thêm `std::string ext_id;`                                                                                                                                                      |
| `src/resident_db.cpp`                            | `load_active`: SELECT thêm cột `ext_id`, gán vào `Resident.ext_id`                                                                                                                           |
| `src/main.cpp`                                   | Khởi tạo`EdgeClient` khi có `--edge-socket` (chỉ ở nhánh `use_resident_db`); trong outcome loop gọi `edge.send_recog(...)` / `edge.send_unknown(...)`; `edge.close()` lúc shutdown |
| `Makefile`                                       | Thêm`edge_client.cpp` + `edge_json.cpp` vào `DB_SRCS` (link vào `face_recog_app`). Không lib mới                                                                                          |
| `tests/test_edge_json.cpp` (mới)                | Unit test serialize + escape                                                                                                                                                                           |
| `tests/test_edge_frame.cpp` (mới)               | Unit test encode header (magic, byte order, payload_len); round-trip qua`socketpair()`                                                                                                               |
| `tests/run_tests.sh`                             | Thêm 2 stanza biên dịch + chạy test mới                                                                                                                                                           |

> **Chiều phụ thuộc**: `edge_json` không phụ thuộc gì (chỉ std). `edge_client` phụ thuộc `edge_json`
>
> + POSIX socket + logger. `main.cpp` phụ thuộc `edge_client` + đọc `Resident.ext_id`. `edge_client`
>   KHÔNG phụ thuộc `resident_db` — main truyền sẵn các giá trị (id string, name, floor) vào
>   `send_recog`, nên module socket độc lập, dễ test.

## 3. Định dạng khung (Framing) — R2

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

## 4. Serializer JSON tối giản — R3

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
Dùng `snprintf(buf, "%.2f", v)`.

### Dải tag v2

Spec dùng **dải tag v2** (đã chốt): `cabin_id` = `20001`, các tag v1 từ `20001` trở đi dời +1. Xem
bảng đối chiếu đầy đủ ở `requirements.md` §4. Tóm tắt các tag AI Core dùng ở chiều gửi:

| Tag       | Ý nghĩa                              | Kiểu  |
| --------- | -------------------------------------- | ------ |
| `20000` | timestamp ms                           | int64  |
| `20001` | cabin_id                               | int    |
| `20002` | mã NV / ID                            | string |
| `20003` | họ tên                               | string |
| `20005` | tầng mặc định                      | int    |
| `20008` | độ tương đồng nhận diện (0..1) | float  |

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

## 5. `EdgeClient` — vòng đời & thread nền (R4, R5, R6)

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

## 6. Móc vào `main.cpp` — R7, R8

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

## 7. Kiểm thử

Cả hai suite build bằng `g++ -std=c++17 -Isrc`, không cần NPU/OpenCV/sqlite — thêm vào
`tests/run_tests.sh`.

**`test_edge_json.cpp`** (R3):

- `json_escape`: `"`, `\`, `\n`, control `\u0001`; UTF-8 tiếng Việt giữ nguyên byte.
- `0x1001` body: đúng thứ tự/kiểu tag (`20000/20001/20002/20003/20005/20008`), `20008` format
  `%.2f`, không có `20007`.
- `0x1002` body: `20003="UNKNOWN_FACE"` (literal), `20008` format `%.2f` bất kể locale, có `20001`.
- `kvi` int64 biên (âm, lớn).

**`test_edge_frame.cpp`** (R2, + transport qua socketpair):

- `encode_frame`: byte 0/1 = 0xAA/0x55; byte 2–3 = `ntohs` ra đúng msg_type; byte 4–7 = `ntohl` ra
  đúng `body.size()`; tổng độ dài = 8 + body.
- Round-trip: tạo `socketpair(AF_UNIX, SOCK_STREAM)`, một đầu `write_all(frame)`, đầu kia đọc header
  8 byte + `payload_len` byte body, so khớp bằng nguyên gói (kiểm tra tách gói đúng khi ghi 2 frame
  liền nhau — chống dính gói).
- Partial write: (tùy chọn) dùng buffer nhỏ để ép `send` một phần, xác nhận `write_all` lặp đủ.

> Reconnect/backoff và bounded-queue drop-oldest test được bằng cách trỏ tới path không tồn tại
> (connect fail) + bơm nhiều job rồi kiểm `dropped_ > 0` — không cần Edge Core thật.

## 8. Quyết định mở & rủi ro

- **`cabin_id` trong payload**: đã chốt dùng tag số `20001` (dải tag v2). `send_recog`/`send_unknown`
  KHÔNG nhận `cabin_id` làm tham số — hàm dựng body lấy từ `EdgeConfig.cabin_id` (đã cấu hình lúc
  khởi tạo `EdgeClient`), nên mọi payload tự động mang đúng cabin. Việc cập nhật file `.docx` theo
  dải v2 do đội tài liệu làm (ngoài scope code).
- **`SO_SNDTIMEO` giá trị bao nhiêu**: mặc định đề xuất ~500 ms; đủ ngắn để không kẹt frame-độc-lập
  (đằng nào cũng ở thread nền) nhưng tránh coi nhầm mạng chậm là lỗi.
- **`confidence` cho unknown**: tài liệu dùng "confidence" (độ tin cậy là mặt người), nhưng AI Core
  hiện chỉ có `similarity` (độ giống người đã biết). Đợt a gửi tạm `similarity`; nếu Edge Core cần
  face-detection score thật thì bổ sung sau (không đổi khung).
- **Đợt b (receiver) sẽ cần**: một read-loop tách frame (đọc 8 byte header → `payload_len` → body),
  parse `0x2001`/`0x2002`, và với `0x2002` phải trích embedding từ `20006` (đường dẫn ảnh, dải v2)
  rồi cập nhật
  MatchEngine/DB. Thiết kế hiện tại tách `transport` (encode/`write_all`) khỏi `worker_loop` nên
  thêm `read_frame` + dispatch là mở rộng thuận, không đập lại.
