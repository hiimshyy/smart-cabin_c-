# WebSocket Migration Review — thay `edge-core-uds`

**Ngày review:** 2026-10-06 · **Cập nhật quyết định:** 2026-10-08
**Nguồn kiểm chứng:** `socket_test.cpp`  
**Trạng thái:** AI Core trực tiếp gọi tầng; endpoint/auth/ack còn cần đồng bộ với IoT Core

## 1. Kết luận

`socket_test.cpp` mô tả một protocol khác hoàn toàn so với spec UDS hiện tại. Đây không phải thay
`AF_UNIX` bằng TCP rồi giữ nguyên payload. Stack mới gồm:

```text
TCP
  └── WebSocket RFC 6455
        └── Engine.IO v4 (text packets)
              └── Socket.IO v4 (event + ack)
                    └── iot_core `Client/command`
```

Vì vậy các phần UDS sau không được mang sang transport mới:

- header nhị phân 8 byte `AA 55 + msg_type + payload_len`;
- message type `0x1001/0x1002/0x1003` và `0x2001/0x2002`;
- heartbeat JSON tag-số của AI Core;
- `--edge-socket` pathname/abstract namespace;
- giả định chỉ có chiều AI→Edge.

Code release hiện tại vẫn có UDS. Migration cần client Socket.IO mới; kết nối full-duplex để xử lý
ping/pong và ack, còn lệnh nghiệp vụ đi theo chiều AI Core → IoT Core. Không nên sửa chắp vá
`EdgeClient` thành WebSocket vì lifecycle và parser khác nhau.

**Quyết định đã chốt ngày 2026-10-08:** sau confirmed recognition, AI Core lấy
`Resident.home_floor` và trực tiếp gửi Socket.IO `Client/command` tới IoT Core. Unknown hoặc resident
không có tầng (`home_floor <= 0`) không phát lệnh. AI Core hiện không subscribe register và không xử
lý `CONN_DATA`.

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

Luồng subscription này thuộc phần đọc trạng thái IoT, chưa thuộc AI Core trong quyết định hiện tại.
Nó được giữ trong review để giải thích sample, không phải requirement cho `SocketIoClient`.

### 2.4 Gửi command và nhận ack

Sample gửi:

```text
420["Client/command", {
  "action": "write",
  "deviceCode": "2026010900100098",
  "address": "8193-2",
  "value": 32,
  "requestId": "ai-core"
}]
```

Phân rã prefix:

- `4`: Engine.IO message;
- `2`: Socket.IO event;
- `0`: ack id bằng 0.

Ack tương ứng bắt đầu `430[...]`: Engine.IO message `4`, Socket.IO ACK `3`, ack id `0`. Production
không được hardcode id 0 nếu có nhiều command đồng thời; cần counter và pending map.

Đây là contract gọi tầng của AI Core. Ack id Socket.IO phải tăng cho từng lệnh; gói đầu có id `0`
nên prefix là `420`, nhưng không được hardcode `420` cho mọi lệnh.

## 3. Ma trận thay đổi so với spec UDS

| Hạng mục | Spec/code hiện tại | WebSocket sample | Cần cập nhật |
|---|---|---|---|
| Service đích | Edge Core UDS server | `iot_core` | AI kết nối IoT Core và trực tiếp gửi lệnh gọi tầng |
| Endpoint | pathname hoặc `@abstract` | host + port + Socket.IO path | Config mới, validate host/port/path |
| Transport | `AF_UNIX/SOCK_STREAM` | TCP + RFC 6455 | Client mới |
| Framing | header 8 byte | WebSocket frames | Bỏ `encode_frame` cho transport mới |
| App protocol | msg type số + tag JSON | Engine.IO/Socket.IO text event | Codec/state machine mới |
| Direction | chủ yếu AI→Edge | AI gửi command, nhận protocol control + ack | Business outbound, protocol full-duplex |
| Heartbeat | app gửi `0x1003` | Engine.IO ping/pong + WS ping/pong | Hai tầng heartbeat đúng chuẩn |
| Reconnect | UDS backoff | TCP + full handshake | Kết nối lại namespace; không resend lệnh có trạng thái mơ hồ |
| Queue | drop-oldest mọi job | Command có ack | Queue bounded, từ chối khi đầy, bỏ lệnh quá hạn |
| Config | `--edge-socket` | URL + deviceCode + address + requestId | CLI/env mới, không hardcode vào codec |
| Tests | header/socketpair | protocol transcript + fake server | Thay test transport |
| Security | file permission UDS | TCP network scope | Bind scope, auth, WSS, secret redaction |

## 4. Quyết định nghiệp vụ và phần còn phải đồng bộ

### D1 — ĐÃ CHỐT: AI Core trực tiếp gửi lệnh gọi tầng

Sau confirmed recognition, `value = 2^Resident.home_floor`. Ví dụ resident có `home_floor=5`:

```text
420["Client/command",{
  "action":"write",
  "deviceCode":"2026010900100098",
  "address":"8193-2",
  "value":32,
  "requestId":"ai-core"
}]
```

`420` được phân rã thành Engine.IO message `4`, Socket.IO event `2`, ack id `0`. Lệnh tiếp theo phải
dùng ack id khác (`421`, `422`, ...). Giá trị `32 = 2^5`, vì vậy lệnh trên gọi tầng 5. Unknown và
resident có `home_floor <= 0` không gửi command. Mapper chỉ nhận tầng 1–62 để phép shift luôn nằm
trong số nguyên 64-bit dương; dải tầng vận hành vẫn phải được kiểm tra theo cấu hình cabin.

### D2 — Endpoint và cấu hình lệnh

AI Core cần WebSocket URL, namespace, optional auth, `deviceCode`, `address` và `requestId`.
Giá trị hiện tại:

- `deviceCode = "2026010900100098"`;
- `address = "8193-2"` (thang máy/slave số 2);
- `requestId = "ai-core"`.

Ba giá trị này thuộc cấu hình deployment để cabin khác có thể override; codec không hardcode.

### D3 — Ack, timeout và trạng thái không rõ

Cần schema chính thức của `status`, `mess`, error code và payload result. Trước khi có idempotency key
duy nhất cho từng lệnh, client áp dụng:

- chỉ một command in-flight;
- ack id tăng cho từng command;
- lệnh nằm trong queue quá 5 giây bị loại để tránh gọi tầng muộn;
- timeout ack hoặc mất kết nối sau khi đã gửi được xem là thất bại có trạng thái không rõ;
- không tự resend command có trạng thái không rõ, vì `requestId="ai-core"` cố định không đủ để
  receiver deduplicate từng lệnh.

Nếu IoT Core cho phép `requestId` duy nhất theo lệnh và cam kết deduplicate, có thể nâng cấp sang
at-least-once retry sau.

### D4 — Security/deployment

Sample chỉ có TCP plaintext và không auth. Production cần chốt:

- chỉ bind loopback hay truy cập qua LAN;
- `ws://` hay `wss://`;
- token/header/query authentication;
- CA/certificate policy;
- log redaction cho URL/token/payload PII.

### D5 — Hiện tại AI Core không dùng `CONN_DATA`

AI Core không gửi `CONN_SUBSCRIBE` và không giữ register snapshot. WebSocket client chỉ xử lý
Engine.IO/Socket.IO lifecycle, gửi `Client/command`, nhận ack và báo lỗi. Nếu sau này cần chặn lệnh
theo cửa/chế độ khẩn cấp/trạng thái board, phải thêm contract trạng thái chuẩn hóa riêng.

## 5. Yêu cầu production đề xuất

### WS1 — Phân lớp

Tách ít nhất ba lớp:

1. `WebSocketCodec`: handshake + RFC 6455 frame/control/fragmentation.
2. `SocketIoSession`: Engine.IO open/ping/pong + namespace + event/ack.
3. `SocketIoClient`: queue floor command, ack/timeout và reconnect.

`main.cpp` chỉ gọi API nghiệp vụ; không ghép prefix `42/420` hoặc JSON bằng nối chuỗi.

### WS2 — Non-blocking

DNS, connect, handshake, read, write, reconnect, resize parser và ack handling chạy ở I/O thread.
Frame/inference thread chỉ enqueue giá trị `home_floor`. Queue phải bounded.

### WS3 — Protocol correctness

- Validate HTTP 101, `Upgrade`, `Connection` và `Sec-WebSocket-Accept`.
- Client frame bắt buộc mask bằng key không lặp dễ đoán.
- Giới hạn payload trước khi allocate.
- Hỗ trợ packet bị chia qua nhiều TCP read và nhiều WebSocket fragment.
- Control frame phải tuân FIN/length; trả ping/pong/close đúng hạn.
- Không truy cập `msg[0]` khi message rỗng.

### WS4 — Session/reconnect

Mỗi reconnect phải chạy lại toàn bộ handshake → namespace. Lệnh chưa từng gửi có thể giữ trong queue
nếu chưa quá hạn. Lệnh đã gửi nhưng chưa ack không tự resend theo D3. Không subscribe register hay
rebuild snapshot. Backoff có jitter và shutdown phải interruptible.

### WS5 — Floor-command delivery

Mỗi command có ack id riêng và deadline. Chỉ cho phép một command in-flight để correlation rõ ràng.
Queue overflow phải reject có counter và log rate-limited. Ack hiện xác nhận IoT Core đã trả lời;
schema success/error vẫn cần chốt để phân biệt nhận packet với ghi board thành công.

### WS6 — Trigger và payload

Chỉ gửi từ confirmed interaction outcome và tối đa một lần cho outcome đó. `home_floor` phải lớn hơn
`HOME_FLOOR_UNSET`; command dùng `value = 2^home_floor`. Unknown không gửi command. Payload đúng D1,
không chứa resident name, ảnh, similarity hoặc dữ liệu khuôn mặt.

### WS7 — Observability

Log state transition/connect/reconnect/ack timeout và counters; không log password, token hoặc
resident name ở INFO. Có counters connected duration, reconnect, queue depth, dropped/rejected,
command sent/acked/failed và parse error.

## 6. Test cần thay đổi/thêm

1. RFC 6455 handshake accept validation, client masking, 7/16/64-bit length, fragmentation,
   ping/pong/close, malformed/control payload và max payload.
2. Engine.IO transcript: open, namespace, ping/pong và reconnect namespace.
3. Contract test chính xác packet `420["Client/command",...]`, escaping, ack id tăng và ack timeout.
4. Command queue: overflow, hết hạn và không resend trạng thái mơ hồ sau disconnect.
5. Fake IoT Core: nhận command, trả ack/error, disconnect trước/sau ack, reconnect và shutdown.
6. Contract test từ transcript thật đã redacted; không commit token/credential/device secret.
7. Live test xác nhận resident `home_floor=5` tạo đúng một write `8193-2=32`.

## 7. Migration và rollback

1. Giữ UDS client hiện tại dưới mode `uds` trong một release.
2. Thêm mode `socketio` độc lập; mặc định vẫn là mode đang vận hành cho tới khi contract/live test
   đạt.
3. Không bật UDS và Socket.IO đồng thời cho cùng một confirmed outcome để tránh service cũ và AI
   cùng gọi tầng.
4. Sau soak, chuyển default rồi mới xóa `edge_json`, header framing và `--edge-socket` ở release sau.
5. `socket_test.cpp` tiếp tục là probe transport/integration của `iot_core`; production AI client
   tái sử dụng contract `Client/command`, nhưng không copy subscribe/register parser phẳng/regex
   hoặc global fd/state vào app.
