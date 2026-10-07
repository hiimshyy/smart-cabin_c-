# WebSocket Migration Review — thay `edge-core-uds`

**Ngày review:** 2026-10-06  
**Nguồn kiểm chứng:** `socket_test.cpp`  
**Trạng thái:** hướng nghiệp vụ đã chốt; endpoint/auth/ack còn cần đồng bộ với service nhận event

## 1. Kết luận

`socket_test.cpp` mô tả một protocol khác hoàn toàn so với spec UDS hiện tại. Đây không phải thay
`AF_UNIX` bằng TCP rồi giữ nguyên payload. Stack mới gồm:

```text
TCP
  └── WebSocket RFC 6455
        └── Engine.IO v4 (text packets)
              └── Socket.IO v4 (event + ack)
                    └── sample: iot_core events/commands
                        target: AI Core face events
```

Vì vậy các phần UDS sau không được mang sang transport mới:

- header nhị phân 8 byte `AA 55 + msg_type + payload_len`;
- message type `0x1001/0x1002/0x1003` và `0x2001/0x2002`;
- heartbeat JSON tag-số của AI Core;
- `--edge-socket` pathname/abstract namespace;
- giả định chỉ có chiều AI→Edge.

Code hiện tại vẫn là UDS và vẫn được test. Migration cần client Socket.IO mới; kết nối full-duplex
để xử lý ping/pong và ack, còn dữ liệu nghiệp vụ của AI Core chỉ đi theo chiều phát face event.
Không nên sửa chắp vá `EdgeClient` thành WebSocket vì lifecycle và parser khác nhau.

**Quyết định đã chốt:** AI Core chỉ phát face event qua Socket.IO. Service nhận event chịu trách
nhiệm quyết định tầng và giao tiếp với `iot_core`/board. AI Core không subscribe register, không xử
lý `CONN_DATA` và không gửi `Client/command`.

## 2. Contract quan sát được từ sample

### 2.1 Endpoint và handshake

| Thuộc tính | Giá trị trong sample |
|---|---|
| Host | `HOST`, mặc định `127.0.0.1` |
| Port | `PORT`, mặc định `3000` |
| Path | `/socket.io/?EIO=4&transport=websocket` |
| WebSocket version | RFC 6455 version 13 |
| Engine.IO | v4 |
| Socket.IO | v4 text protocol |
| Namespace | `/` |
| TLS/auth | Chưa có trong sample |

Client gửi WebSocket frame có masking. Server frame được parse theo length 7/16/64 bit. Sample gộp
continuation frames và trả lời WebSocket ping bằng pong.

### 2.2 State machine khi kết nối

```text
TCP connect
  → HTTP Upgrade 101
  → receive Engine.IO open: "0{...}"
  → send "40"                         // connect namespace /
  → receive "40..."                   // namespace connected
  → send 42["CONN_SUBSCRIBE", {sn, level}]
  → receive 42["CONN_SUBSCRIBE_RESULT", ...]
  → receive 42["CONN_DATA", ...]      // stream hai chiều đã sẵn sàng
```

Engine.IO ping text `"2"` phải được trả bằng `"3"`. Đây là heartbeat do server điều phối; không gửi
`0x1003`/JSON heartbeat của UDS bên trong Socket.IO.

### 2.3 Subscription và state dữ liệu trong sample

Request:

```json
["CONN_SUBSCRIBE", {"sn":"<SN>", "level":"<LEVEL>"}]
```

Các packet Socket.IO thực tế có prefix Engine.IO/Socket.IO `42`. `CONN_DATA` chứa `{sn, level,
data}`. State cần merge theo rule:

- packet đầu thường là `Mode="full"`;
- `Mode="new"` chỉ chứa register vừa đổi và phải merge vào snapshot hiện tại;
- nhiều slave dùng key `<register>-<slave>`, ví dụ `8465-1`;
- `Mode="full"` có `ADDR=N` chỉ thay nhóm key của slave `N`, không xóa slave khác;
- full packet không có `ADDR` mới được thay toàn bộ snapshot.

Sample cho biết `8465-1` là ví dụ register tầng hiện tại. Ý nghĩa/calibration của toàn bộ register
phải lấy từ register table chính thức, không hardcode từ một ví dụ.

Luồng này thuộc service điều phối/IoT, không thuộc AI Core sau quyết định face-event-only. Nó được
giữ trong review để giải thích sample, không phải requirement cho `FaceEventSocketClient`.

### 2.4 Gửi command và nhận ack

Sample gửi:

```text
420["Client/command", {
  "action": "write",
  "deviceCode": "<SN>",
  "address": "8193-1",
  "value": 2,
  "requestId": "cpp-test"
}]
```

Phân rã prefix:

- `4`: Engine.IO message;
- `2`: Socket.IO event;
- `0`: ack id bằng 0.

Ack tương ứng bắt đầu `430[...]`: Engine.IO message `4`, Socket.IO ACK `3`, ack id `0`. Production
không được hardcode id 0 nếu có nhiều command đồng thời; cần counter và pending map.

AI Core không gửi command này. Cơ chế ack id của Socket.IO vẫn được tái sử dụng để xác nhận service
đã nhận face event.

## 3. Ma trận thay đổi so với spec UDS

| Hạng mục | Spec/code hiện tại | WebSocket sample | Cần cập nhật |
|---|---|---|---|
| Service đích | Edge Core UDS server | Sample kết nối `iot_core` | AI kết nối service nhận face event; service đó sở hữu quyết định tầng |
| Endpoint | pathname hoặc `@abstract` | host + port + Socket.IO path | Config mới, validate host/port/path |
| Transport | `AF_UNIX/SOCK_STREAM` | TCP + RFC 6455 | Client mới |
| Framing | header 8 byte | WebSocket frames | Bỏ `encode_frame` cho transport mới |
| App protocol | msg type số + tag JSON | Engine.IO/Socket.IO text event | Codec/state machine mới |
| Direction | chủ yếu AI→Edge | AI emit face event, nhận protocol control + ack | Business outbound, protocol full-duplex |
| Heartbeat | app gửi `0x1003` | Engine.IO ping/pong + WS ping/pong | Hai tầng heartbeat đúng chuẩn |
| Reconnect | UDS backoff | TCP + full handshake | Kết nối lại namespace rồi resend face event chưa ack |
| Queue | drop-oldest mọi job | Socket.IO event có ack | Queue face event bounded, retry theo `eventId` |
| Config | `--edge-socket` | WebSocket URL + auth; cabin id nằm trong event | Schema/CLI/env mới |
| Tests | header/socketpair | protocol transcript + fake server | Thay test transport |
| Security | file permission UDS | TCP network scope | Bind scope, auth, WSS, secret redaction |

## 4. Quyết định nghiệp vụ và phần còn phải đồng bộ

### D1 — ĐÃ CHỐT: AI Core chỉ phát face event

Hai Socket.IO event canonical:

```text
FACE_RECOGNIZED
FACE_UNKNOWN
```

AI Core emit event có Socket.IO ack id:

```text
42<ackId>["FACE_RECOGNIZED", {...}]
42<ackId>["FACE_UNKNOWN", {...}]
```

Payload `FACE_RECOGNIZED`:

```json
{
  "eventId": "unique-stable-id",
  "timestampMs": 1791234567890,
  "cabinId": 1,
  "trackId": 27,
  "residentId": 123,
  "subjectId": "external-id-or-resident-id",
  "homeFloor": 6,
  "similarity": 0.49
}
```

Payload `FACE_UNKNOWN`:

```json
{
  "eventId": "unique-stable-id",
  "timestampMs": 1791234567890,
  "cabinId": 1,
  "trackId": 28,
  "similarity": -1.0
}
```

`timestampMs` bắt buộc là Unix epoch milliseconds từ `system_clock`, không dùng monotonic
`steady_clock`. `eventId` phải ổn định qua reconnect/retry để receiver deduplicate. Payload không
gửi display name/greeting vì service quyết định tầng chỉ cần định danh, cabin, floor và confidence.

Service nhận event mới quyết định có gọi tầng hay không. AI Core không có API `Client/command`,
không biết register/slave và không tự ghi board.

### D2 — Endpoint và routing service

AI Core chỉ cần WebSocket URL, namespace, optional auth và `cabinId`. `sn`, `level`, `deviceCode`,
register/slave (`8193-1`) thuộc service điều phối và không được thêm vào AI config.

### D3 — Ack, retry và idempotency

Cần schema chính thức của `status`, `mess`, error code và payload result. Chốt:

- ack timeout;
- retry bao nhiêu lần;
- reconnect có resend face event chưa ack hay không;
- receiver trả ack success/error theo schema nào;
- face event hết hạn sau bao lâu để tránh service xử lý một sự kiện quá cũ.

`eventId` là idempotency key bắt buộc: AI Core giữ nguyên giá trị khi retry và receiver dùng giá trị
này để deduplicate.

### D4 — Security/deployment

Sample chỉ có TCP plaintext và không auth. Production cần chốt:

- chỉ bind loopback hay truy cập qua LAN;
- `ws://` hay `wss://`;
- token/header/query authentication;
- CA/certificate policy;
- log redaction cho URL/token/payload PII.

### D5 — ĐÃ CHỐT: AI Core không dùng `CONN_DATA`

AI Core không gửi `CONN_SUBSCRIBE` và không giữ register snapshot. Phần full/new/ADDR trong sample
thuộc service điều phối. WebSocket client của AI chỉ xử lý Engine.IO/Socket.IO lifecycle, ack và lỗi.

## 5. Yêu cầu production đề xuất

### WS1 — Phân lớp

Tách ít nhất ba lớp:

1. `WebSocketCodec`: handshake + RFC 6455 frame/control/fragmentation.
2. `SocketIoSession`: Engine.IO open/ping/pong + namespace + event/ack.
3. `FaceEventSocketClient`: queue, emit face event, ack/dedupe và reconnect.

`main.cpp` chỉ gọi API nghiệp vụ; không ghép prefix `42/420` hoặc JSON bằng nối chuỗi.

### WS2 — Non-blocking

DNS, connect, handshake, read, write, reconnect, resize parser và ack handling chạy ở I/O thread.
Frame/inference thread chỉ enqueue immutable face event. Queue phải bounded.

### WS3 — Protocol correctness

- Validate HTTP 101, `Upgrade`, `Connection` và `Sec-WebSocket-Accept`.
- Client frame bắt buộc mask bằng key không lặp dễ đoán.
- Giới hạn payload trước khi allocate.
- Hỗ trợ packet bị chia qua nhiều TCP read và nhiều WebSocket fragment.
- Control frame phải tuân FIN/length; trả ping/pong/close đúng hạn.
- Không truy cập `msg[0]` khi message rỗng.

### WS4 — Session/reconnect

Mỗi reconnect phải chạy lại toàn bộ handshake → namespace. Sau khi namespace sẵn sàng, client gửi
lại các face event chưa ack với nguyên `eventId`; không subscribe register hay rebuild snapshot.
Backoff có jitter và shutdown phải interruptible.

### WS5 — Face-event delivery

Mỗi face event có ack id và `eventId` unique/stable. Pending map lưu deadline. Delivery là
at-least-once: event chưa ack được gửi lại sau reconnect, receiver deduplicate theo `eventId`. Queue
overflow không được âm thầm drop; phải coalesce/reject có counter và log rate-limited.

### WS6 — Face-event schema

Schema phải đúng D1, dùng named fields, finite similarity và Unix epoch ms. `FACE_RECOGNIZED` chỉ
phát từ confirmed interaction outcome; `FACE_UNKNOWN` chỉ phát sau `unknown_after_ms`. Không phát
event trực tiếp từ detection frame và không gửi display name/greeting.

### WS7 — Observability

Log state transition/connect/reconnect/ack timeout và counters; không log password, token hoặc
resident name ở INFO. Có counters connected duration, reconnect, queue depth, dropped/rejected,
face event sent/acked/timeout/retried và parse error.

## 6. Test cần thay đổi/thêm

1. RFC 6455 handshake accept validation, client masking, 7/16/64-bit length, fragmentation,
   ping/pong/close, malformed/control payload và max payload.
2. Engine.IO transcript: open, namespace, ping/pong và reconnect namespace.
3. Socket.IO face event/ack: schema, nhiều ack id đồng thời, ack out-of-order, timeout và late ack.
4. Event delivery: reconnect resend, receiver dedupe `eventId`, queue overflow/coalesce.
5. Fake face-event service: disconnect trước/sau ack, reconnect và shutdown.
6. Contract test từ transcript thật đã redacted; không commit token/credential/device secret.
7. Live test xác nhận service nhận đúng event và AI Core không phát `Client/command`.

## 7. Migration và rollback

1. Giữ UDS client hiện tại dưới mode `uds` trong một release.
2. Thêm mode `socketio` độc lập; mặc định vẫn là mode đang vận hành cho tới khi contract/live test
   đạt.
3. Không bật UDS và Socket.IO đồng thời cho cùng một business action để tránh gửi trùng.
4. Sau soak, chuyển default rồi mới xóa `edge_json`, header framing và `--edge-socket` ở release sau.
5. `socket_test.cpp` tiếp tục là probe transport/integration của `iot_core`; production AI client
   chỉ tái sử dụng kiến thức RFC/Engine.IO/Socket.IO, không copy subscribe/register/command logic,
   parser phẳng/regex hoặc global fd/state vào app.
