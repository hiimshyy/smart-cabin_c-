# Requirements — AI Core ↔ Edge Core qua Unix Domain Socket (chiều AI→Edge)

**Spec ID**: `edge-core-uds` · **Đề xuất số**: 5 (giao tiếp nội bộ Edge)
**Phụ thuộc**: `resident-db-layer` (ResidentDB, `Resident`, `MatchEvent`), `main.cpp` (vòng lặp
outcome nhận diện), `cabin-runtime-config` (AppConfig, precedence CLI)
**Liên quan**: `docs/Smart_cabin_communicate.docx` (§3 Luồng giao tiếp nội bộ Edge),
`DEVELOPMENT_PLAN.md` §2 (topology), §6 (gọi tầng qua ConnCore/COMM Core)

## 1. Bối cảnh & vấn đề

`face_recog_app` (dự án này) chính là **AI Core** trong sơ đồ Smart Cabin: tiến trình chạy model
inference (Face Detection → Alignment → Recognition). Hôm nay, sau khi nhận diện thành công một
khuôn mặt, hành động DUY NHẤT của app là:

- ghi một dòng `match_events` vào SQLite (async, qua `ResidentDB::log_event`), và
- in một dòng `LOG_INFO("event", "CONFIRMED resident_id=...")`.

Không có bất kỳ code socket/IPC/mạng nào trong `src/` — app chưa thông báo cho ai khác biết "vừa
nhận diện được người này". Nhưng để cabin **thật sự gọi thang**, sự kiện nhận diện phải đi tới
**Edge Core** — tiến trình trung tâm quản lý logic thang máy, nói chuyện với **COMM Core** (Modbus
RTU) để ghi lệnh gọi tầng xuống board thang.

Tài liệu `Smart_cabin_communicate.docx` §3 đã chốt giao thức giao tiếp nội bộ Edge:

- **Kênh**: Unix Domain Socket (UDS), `SOCK_STREAM`, full-duplex.
- **Vai trò**: **Edge Core = SERVER** (lắng nghe trên `/run/elev_edge_core.sock` hoặc abstract
  namespace `@elev_edge_core.sock`). **AI Core = CLIENT** (chủ động `connect()`).
- **Đóng gói (framing)**: header 8 byte cố định + body JSON UTF-8, để chống dính gói/vỡ gói trên
  luồng byte `SOCK_STREAM`.

Vấn đề bản chất: UDS `SOCK_STREAM` là **luồng byte liên tục**. Nếu AI Core gửi nhiều gói liên tiếp,
một lần `read()` phía Edge Core có thể nhận nhiều gói dính liền, hoặc chỉ nhận được một phần JSON.
Phải có khung độ-dài-cố-định để tách gói chính xác.

Spec này chốt **chiều AI Core → Edge Core** (đợt "a"): AI Core kết nối UDS, đóng khung, và gửi
3 loại thông điệp — `EVENT_FACE_RECOG_DETECTED` (0x1001), `EVENT_FACE_UNKNOWN` (0x1002),
`CMD_HEARTBEAT_PING` (0x1003) — mà **không** làm chậm vòng lặp nhận diện realtime.

## 2. Phạm vi

**Trong scope (đợt a — chỉ chiều AI→Edge):**

1. Module `EdgeClient` (mới): UDS client `SOCK_STREAM`, `connect()` tới socket của Edge Core, tự
   kết nối lại (reconnect) khi Edge Core chưa lên hoặc rớt kết nối.
2. Đóng khung length-prefixed: header 8 byte (`magic` `0xAA 0x55` + `msg_type` uint16 +
   `payload_len` uint32 network-byte-order) + body JSON UTF-8.
3. Serializer JSON tối giản (tự viết, không thêm dependency) cho các payload tag-số.
4. Gửi 3 thông điệp chiều AI→Edge:
   - `0x1001 EVENT_FACE_RECOG_DETECTED` — khi nhận diện được người (confirmed).
   - `0x1002 EVENT_FACE_UNKNOWN` — khi phát hiện người lạ (unknown).
   - `0x1003 CMD_HEARTBEAT_PING` — giữ kết nối định kỳ.
5. Gửi **non-blocking** với thread nền + hàng đợi (mirror pattern async của `ResidentDB`): vòng lặp
   frame KHÔNG được block trên socket.
6. Cấu hình đường dẫn socket qua CLI flag mới `--edge-socket` (precedence CLI > default; không đọc
   từ DB ở đợt này).
7. Nạp thêm `residents.ext_id` vào struct `Resident` để điền tag `20002`.
8. Móc vào vòng lặp outcome ở `main.cpp` (nhánh `confirmed` → 0x1001, nhánh `unknown` → 0x1002).

**Ngoài scope:**

- **Chiều Edge→AI** (đợt "b"): nhận `0x2001 RESP_CALL_FLOOR_RESULT` và
  `0x2002 CMD_FACE_REGISTER_REQUEST`. Đợt này chỉ ghi—không—đọc; receiver để dành spec/đợt sau.
  `EdgeClient` PHẢI thiết kế để mở rộng receiver về sau mà không đập lại kiến trúc.
- **Tag `20007` (tầng người dùng bấm tay)**: AI Core KHÔNG có nguồn dữ liệu này (nút bấm đến từ board
  tầng qua Modbus → COMM Core → Edge Core). Đợt a **không gửi** `20007`. Ghép "người vừa nhận diện"
  với "nút tầng vừa bấm" là bài toán tương quan của Edge Core (xem §3, R7).
- **Ra quyết định gọi tầng / chống-ghi-đè bấm tay**: thuộc Edge Core, không phải AI Core.
- **Cấu hình socket path qua DB `cabins`**: đợt này chỉ CLI + default. Không migration schema.
- **MQTT / Cloud** (§2 tài liệu): thuộc thành phần khác, không đụng ở spec này.
- **Mã hóa / xác thực trên UDS**: UDS cục bộ trên cùng máy, quyền truy cập theo file-permission của
  socket; không thêm TLS/token ở v1.

## 3. Phân tách trách nhiệm AI Core vs Edge Core (quyết định trung tâm)

Đây là quyết định kiến trúc quan trọng nhất của spec, trả lời câu hỏi "làm thế nào để một người đổi
tầng bằng tay không ảnh hưởng gọi tầng tự động của người khác".

**Nguyên tắc**: AI Core chỉ **nhận diện và báo cáo sự kiện cá nhân**; Edge Core **điều phối và ra
quyết định** gọi tầng.

- **AI Core (spec này)** phát ra sự kiện *"người X (tầng mặc định Y) vừa được nhận diện lúc T"*. Mỗi
  sự kiện gắn định danh cá nhân (`20002`) + tầng mặc định của chính người đó (`20005`) + timestamp
  (`20000`) + cabin (`20001`). AI Core **không** giữ biến "tầng cabin" toàn cục, nên **không có
  trạng thái chung để bị ghi đè**. Nhiều người trong cabin ⇒ nhiều sự kiện độc lập, mỗi sự kiện một
  danh tính.
- **Edge Core (ngoài scope)** nhận các sự kiện đó, gộp vào hàng đợi đích đến của thang (cộng dồn, KHÔNG
  thay thế). "Bấm tay" của một người chỉ tác động phần đóng góp của chính người bấm, không đụng tầng
  của người khác. Logic này thực thi ở Edge Core.
- Để Edge Core làm được việc tương quan/chống-ghi-đè, mỗi sự kiện AI gửi lên PHẢI đủ **khóa tương
  quan**: `20001` (cabin nào) + `20002` (ai) + `20000` (khi nào). Xem R7.

## 4. Từ điển tag số (Data Dictionary) — dải tag v2

**Thay đổi so với `Smart_cabin_communicate.docx` (dải v1)**: chèn `cabin_id` vào `20001`, mọi tag từ
`20001` (cũ) trở đi **dời +1**. Bảng dưới là **hợp đồng chính thức** của spec này (đã chốt với chủ
dự án). Ngoài việc dời +1, dải v2 còn **bổ sung `20008`** (độ tương đồng nhận diện) cho `0x1001` —
tag này không có trong v1. Tài liệu `.docx` §2 (MQTT) + §3 (UDS) cần đội tài liệu cập nhật theo dải
v2 này để đồng bộ liên-team (xem `tasks.md`). Body JSON dùng key là chuỗi tag số:

| Tag v2 | (v1 cũ) | Ý nghĩa | Kiểu | Nguồn trong AI Core |
|---|---|---|---|---|
| `20000` | `20000` | Epoch timestamp, mili giây | int64 | `now` lúc phát outcome |
| `20001` | *(mới)* | **cabin_id** — cabin phát sự kiện | int | `AppConfig.cabin_id` |
| `20002` | `20001` | Mã nhân viên / ID người dùng | string | `Resident.ext_id`, fallback `to_string(resident_id)` |
| `20003` | `20002` | Họ và tên | string | `Resident.name` |
| `20004` | `20003` | Chức vụ / vai trò | string | (không gửi ở 0x1001; dùng cho 0x2002 chiều vào — ngoài scope) |
| `20005` | `20004` | Tầng mặc định đăng ký | int | `Resident.home_floor` |
| `20006` | `20005` | Đường dẫn ảnh khuôn mặt | string | (chiều vào 0x2002 — ngoài scope) |
| `20007` | `20006` | Tầng người dùng bấm tay | int | **KHÔNG gửi ở đợt a** (nguồn từ Edge Core) |
| `20008` | *(mới)* | Độ tương đồng nhận diện (similarity, 0..1) | float | `Outcome.similarity` (0x1001) |

## 5. Yêu cầu chức năng

### R1 — Kết nối UDS client tới Edge Core
Là AI Core, tôi cần chủ động kết nối tới socket của Edge Core để gửi sự kiện.

1. KHI khởi động ở chế độ `--resident-db` VÀ có `--edge-socket`, HỆ THỐNG PHẢI tạo một socket
   `AF_UNIX` / `SOCK_STREAM` và `connect()` tới đường dẫn được cấu hình.
2. HỆ THỐNG PHẢI hỗ trợ cả hai dạng đường dẫn: **pathname** (ví dụ `/run/elev_edge_core.sock`) và
   **abstract namespace** (đường dẫn bắt đầu bằng `@`, ánh xạ sang leading NUL của `sun_path`).
3. NẾU Edge Core chưa lắng nghe (connect thất bại: `ENOENT`/`ECONNREFUSED`), HỆ THỐNG KHÔNG được
   crash; PHẢI log cảnh báo và thử kết nối lại theo backoff (R5).
4. NẾU không truyền `--edge-socket`, HỆ THỐNG PHẢI chạy đúng như hôm nay (EdgeClient tắt hoàn toàn,
   không tạo thread, không kết nối) — không hồi quy.

### R2 — Đóng khung length-prefixed (Message Framing)
Là bên gửi trên luồng byte `SOCK_STREAM`, tôi cần đóng khung để Edge Core tách gói chính xác.

1. Mỗi thông điệp PHẢI gồm **header 8 byte** + **body**:
   - byte 0–1: magic `0xAA 0x55` (theo đúng thứ tự này trên đường truyền).
   - byte 2–3: `msg_type` (uint16).
   - byte 4–7: `payload_len` (uint32) — độ dài chính xác body, **network byte order** (`htonl`).
2. `msg_type` và `payload_len` PHẢI được ghi theo network byte order (`htons`/`htonl`) để bên nhận
   dùng `ntohs`/`ntohl` bất kể endianness máy.
3. Body PHẢI là chuỗi JSON UTF-8; `payload_len` PHẢI đúng bằng số **byte** của body (không phải số
   ký tự), tính sau khi serialize.
4. HỆ THỐNG PHẢI ghi header + body bằng ghi đầy đủ (`send`/`write` lặp cho tới hết, xử lý
   partial-write) — không giả định một lần `send()` ghi hết.

### R3 — Serialize payload JSON (tự viết, không dependency)
Là AI Core, tôi cần tạo body JSON đúng định dạng tag-số mà không kéo thêm thư viện.

1. HỆ THỐNG PHẢI serialize được object JSON phẳng gồm cặp key(string)–value, value kiểu string hoặc
   số nguyên (int64), khớp từ điển tag §4.
2. Giá trị string PHẢI được escape đúng JSON: ít nhất `"` → `\"`, `\` → `\\`, và các ký tự điều
   khiển < 0x20 (`\n`, `\r`, `\t`, `\uXXXX`). Tên tiếng Việt (UTF-8 nhiều byte) PHẢI được giữ nguyên
   byte (JSON cho phép UTF-8 thô trong string).
3. `0x1001` PHẢI serialize đúng các tag: `20000` (int64), `20001` (int, cabin_id), `20002` (string,
   mã NV/ID), `20003` (string, họ tên), `20005` (int, tầng mặc định), `20008` (float, độ tương đồng
   nhận diện). KHÔNG chứa `20007`.
4. `0x1002` PHẢI serialize: `20000` (int64), `20001` (int, cabin_id), `status` = `"UNKNOWN_FACE"`,
   `confidence` (số thực, ví dụ similarity tốt nhất của phiên; nếu không có thì `0`). (Theo mẫu
   §3.4.3 tài liệu, có bổ sung `20001`.)
5. `0x1003` PHẢI serialize tối thiểu `20000` (int64) làm heartbeat.

### R4 — Gửi non-blocking (không chặn vòng lặp frame)
Là cabin realtime, tôi cần việc gửi sự kiện KHÔNG làm tụt FPS nhận diện.

1. HỆ THỐNG PHẢI cung cấp API fire-and-forget (ví dụ `send_recog(...)`, `send_unknown(...)`) mà
   người gọi (vòng lặp frame) trả về ngay, không đợi I/O socket.
2. Việc thực sự ghi socket PHẢI diễn ra trên **một thread nền riêng** tiêu thụ hàng đợi (mirror
   `ResidentDB::writer_loop` — mutex + condition_variable + queue).
3. NẾU hàng đợi đầy quá ngưỡng (Edge Core rớt lâu, không tiêu thụ), HỆ THỐNG PHẢI **drop gói cũ
   nhất** (bounded queue) + log đếm số gói mất, KHÔNG để hàng đợi phình vô hạn gây OOM.
4. `close()`/shutdown PHẢI flush best-effort rồi join thread nền sạch sẽ (không treo, không rò
   thread), giống `ResidentDB::close()`.
5. Lỗi ghi socket (`EPIPE`/`ECONNRESET`) trên thread nền PHẢI kích hoạt reconnect (R5), KHÔNG làm
   chết thread nền hay chết app.

### R5 — Reconnect bền bỉ
Là AI Core chạy 24/7, tôi cần tự nối lại khi Edge Core restart mà không cần restart AI Core.

1. KHI connect thất bại hoặc kết nối đang mở bị rớt, HỆ THỐNG PHẢI thử kết nối lại theo backoff
   (ví dụ tăng dần tới trần), tái dùng khoảng `reconnect_min_ms`/`reconnect_max_ms` sẵn có nếu hợp
   lý, hoặc hằng số riêng của module.
2. Trong lúc mất kết nối, `send_*` PHẢI vẫn nhận vào hàng đợi (tới giới hạn R4.3) để không mất trắng
   sự kiện khi Edge Core sắp lên lại; gói vượt giới hạn bị drop theo R4.3.
3. HỆ THỐNG PHẢI log chuyển trạng thái kết nối (connected/disconnected/retrying) đủ để field-test
   theo dõi, nhưng KHÔNG spam mỗi lần retry (log có tiết chế).

### R6 — Heartbeat
Là Edge Core, tôi cần biết AI Core còn sống.

1. HỆ THỐNG PHẢI gửi `0x1003 CMD_HEARTBEAT_PING` định kỳ (chu kỳ cấu hình được, mặc định ví dụ
   5000 ms) khi đang có kết nối.
2. Heartbeat PHẢI đi qua cùng đường gửi non-blocking (R4), KHÔNG tạo đường ghi socket song song.
3. NẾU không có kết nối, HỆ THỐNG KHÔNG cần gửi heartbeat (reconnect lo việc nối lại).

### R7 — Nội dung sự kiện đủ cho Edge Core điều phối
Là Edge Core, tôi cần mỗi sự kiện có đủ khóa để tương quan và không ghi đè lẫn nhau (§3).

1. `0x1001` PHẢI chứa `20002` (định danh cá nhân) + `20005` (tầng mặc định của người đó) +
   `20000` (timestamp ms) để Edge Core cộng dồn yêu cầu tầng theo từng người, và `20008` (độ tương
   đồng nhận diện) để Edge Core cân nhắc mức độ tin cậy trước khi auto-call.
2. Sự kiện (`0x1001` và `0x1002`) PHẢI mang `20001` (cabin_id, từ `AppConfig.cabin_id`) trong payload
   để Edge Core biết yêu cầu thuộc cabin nào. (Chốt: đưa vào payload dưới dạng tag số `20001`.)
3. `20002` PHẢI lấy từ `Resident.ext_id`; NẾU `ext_id` rỗng/null, PHẢI fallback về
   `std::to_string(resident_id)` để payload luôn có định danh.
4. AI Core KHÔNG được tự sinh/gửi `20007`; trường này do Edge Core điền khi tương quan với sự kiện
   Modbus bấm tay (§3, ngoài scope).

### R8 — Cấu hình & nạp dữ liệu
Là dev/vận hành, tôi cần bật/tắt và trỏ socket bằng CLI.

1. HỆ THỐNG PHẢI thêm CLI flag `--edge-socket <path>` vào `AppConfig` (mặc định: rỗng = tắt).
2. `usage`/help PHẢI mô tả flag mới.
3. HỆ THỐNG PHẢI nạp thêm cột `residents.ext_id` vào struct `Resident` khi `load_active`, để có sẵn
   `20002` lúc phát sự kiện (không truy vấn DB trong vòng lặp nóng).
4. KHI `--edge-socket` không truyền, toàn bộ đường EdgeClient PHẢI bị tắt (R1.4).

## 6. Yêu cầu phi chức năng

- **Không hồi quy**: không có `--edge-socket` ⇒ hành vi app y hệt hôm nay; không thread mới, không
  I/O mới. Các CLI args + build hiện có giữ nguyên.
- **Không chặn realtime**: đường gửi hoàn toàn non-blocking; vòng lặp frame không bao giờ đợi
  socket (R4). Socket nên đặt phù hợp (ví dụ ghi có timeout/non-blocking) để một Edge Core treo
  không kéo theo AI Core treo.
- **Build**: KHÔNG thêm dependency ngoài. Chỉ dùng POSIX (`<sys/socket.h>`, `<sys/un.h>`, `htonl`)
  và std C++17. `-lpthread`/`-lrt` đã link sẵn. JSON serialize tự viết (tiền lệ
  `parse_first_camera_url`).
- **Quyền riêng tư (R7 logging)**: log của module KHÔNG in tên/PII; dùng `resident_id`/`track_id`
  dạng số. Body JSON gửi cho Edge Core được phép chứa tên/tầng (đó là mục đích nghiệp vụ) — ranh
  giới: PII đi trên socket thì được, PII vào log thì không.
- **Quan sát được**: log trạng thái kết nối, số gói đã gửi/drop, đủ để field-test đối chiếu.
- **Mở rộng được**: kiến trúc `EdgeClient` phải thêm được receiver (chiều Edge→AI, đợt b) sau này
  mà không đập lại — ví dụ tách rõ transport (framing read/write) khỏi logic sender.
- **Kiểm thử được**: framing (encode header) + JSON serialize + escape phải unit-test được **không
  cần socket thật**; transport có thể test qua `socketpair()`.

## 7. Giả định & quyết định

- AI Core = client, Edge Core = server; Edge Core có thể lên sau AI Core ⇒ reconnect là bắt buộc,
  không phải tùy chọn.
- Định danh nội bộ `resident_id` (int64) GIỮ NGUYÊN; `20002` map sang `ext_id` (string) — không đổi
  khóa chính, không phá FK/MatchEngine (quyết định đã chốt với chủ dự án).
- `20007` do Edge Core sở hữu; AI Core không biết nút bấm vật lý.
- **Dải tag v2** (cabin_id=`20001`, các tag khác dời +1) là hợp đồng chốt với chủ dự án; áp dụng nhất
  quán cho cả MQTT (§2 tài liệu) lẫn UDS (§3). Việc cập nhật file `.docx` do đội tài liệu thực hiện
  (ngoài scope code của spec này) — xem `tasks.md`.
- Đợt này một chiều (AI→Edge). Nhận phản hồi (`0x2001`) và lệnh đăng ký (`0x2002`) để đợt b.
- Một AI Core nối tới một Edge Core cục bộ trên cùng Orange Pi; không multiplex nhiều Edge Core.
