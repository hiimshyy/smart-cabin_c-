# Requirements: Điều phối inference theo trạng thái thang máy

Trạng thái: **Draft để chốt hợp đồng event với IoT Core trước khi triển khai**
Phạm vi: pipeline realtime trong cabin gồm YOLO person detection, SCRFD face detection, face
recognition, person tracker và tín hiệu trạng thái thang máy từ IoT Core.

## 1. Mục tiêu

Giảm số lần chạy NPU khi cabin không cần nhận diện nhưng vẫn bảo đảm không bỏ lỡ hành khách và không
làm sai logic gọi tầng. Hệ thống điều chỉnh tần suất inference theo trạng thái cửa, trạng thái kết nối
IoT Core, person track và identity của từng track.

Các model vẫn được load một lần khi khởi động. “Bật/tắt model” trong tài liệu này có nghĩa là cho phép
hoặc bỏ qua một lần chạy inference; không unload/reload NPU context trong runtime.

## 2. Hiện trạng

- YOLO chạy theo `frame_id % person_every == 0`.
- Person tracker predict mỗi frame và update khi YOLO chạy.
- SCRFD chạy trên mọi frame đã nhận được, kể cả khi cabin không có người.
- Face recognition đã được gate một phần qua `tracker->needs_recognition(track_id, now_ms)`.
- Known identity đã latch theo track nên track đã biết không cần recognition lại.
- Socket.IO hiện chủ yếu gửi `Client/command` và nhận ACK; chưa có lớp trạng thái thang máy chuẩn hóa.

Chi phí cần giảm trước tiên là số lần chạy SCRFD khi không có người và số lần chạy YOLO khi cabin
đang idle. Recognition tiếp tục được quản lý theo từng track, không tắt toàn cục sau người đầu tiên.

## 3. Quyết định kiến trúc

1. IoT Core đọc Modbus và chuyển dữ liệu board thành event nghiệp vụ; AI Core không đọc raw Modbus.
2. AI Core nhận event trạng thái qua WebSocket/Socket.IO đang dùng với IoT Core.
3. `ElevatorStateProvider` quản lý snapshot, sequence, timestamp, reconnect và độ mới của state.
4. `InferenceScheduler` là module thuần logic, không gọi NPU, socket hoặc DB.
5. Khi state IoT thiếu hoặc quá cũ, scheduler chuyển `DEGRADED`, tiếp tục quét dự phòng thay vì tắt
   inference.
6. Khi ở maintenance, emergency hoặc fire mode, AI Core không phát lệnh gọi tầng tự động. Quyết định
   ghi hình/nhận diện trong các mode này được cấu hình riêng.

## 4. Event cần nhận từ IoT Core

Schema logic tối thiểu, tên Socket.IO event và envelope cuối cùng cần chốt với IoT Core:

```json
{
  "event": "elevator/state",
  "deviceCode": "2026010900100098",
  "elevator": 2,
  "sequence": 1042,
  "timestamp": 1791760000000,
  "doorState": "opening",
  "currentFloor": 3,
  "direction": "up",
  "moving": false,
  "serviceMode": "normal",
  "callPending": true
}
```

Các trường bắt buộc:

| Trường | Giá trị | Mục đích |
|---|---|---|
| `deviceCode` | string | Loại event của thiết bị khác |
| `elevator` | integer | Chỉ nhận thang máy số 2 |
| `sequence` | uint64 tăng dần | Loại event cũ/đảo thứ tự |
| `timestamp` | epoch milliseconds | Audit và kiểm tra state stale |
| `doorState` | `closed/opening/open/closing/unknown` | Tín hiệu đánh thức chính |
| `serviceMode` | `normal/maintenance/emergency/fire/unknown` | Cho phép hoặc chặn tự động |

Các trường khuyến nghị: `currentFloor`, `direction`, `moving`, `callPending`. `callPending` chỉ dùng để
đánh thức sớm; không được là điều kiện duy nhất để bật/tắt pipeline.

Khi client kết nối hoặc reconnect, IoT Core phải gửi snapshot hiện tại; AI Core không được chờ một
thay đổi tiếp theo mới biết state.

## 5. State machine scheduler

### S0 — `IDLE`

Điều kiện điển hình: cửa đóng, không có active person track và state IoT còn mới.

- Person detection chạy ở tần suất dự phòng thấp.
- SCRFD tắt.
- Recognition tắt.
- Camera capture/decode và tracker clock vẫn hoạt động.

### S1 — `ARMED`

Điều kiện: có `callPending`, cửa `opening`, hoặc scheduler nhận tín hiệu cabin sắp phục vụ.

- Tăng tần suất person detection trước khi cửa mở hoàn toàn.
- SCRFD chỉ chạy sau khi có person detection/track phù hợp.
- Trạng thái tự hết hạn nếu không có cửa mở hoặc person sau `armed_timeout_ms`.

### S2 — `DOOR_ACTIVE`

Điều kiện: cửa `opening`, `open` hoặc `closing` trong grace.

- Person detection chạy tần suất cao.
- SCRFD chạy khi có ít nhất một person track đủ điều kiện.
- Recognition chạy cho từng track chưa có identity latch.

### S3 — `OCCUPIED_UNRESOLVED`

Điều kiện: có person track nhưng còn ít nhất một track chưa xác định.

- Person detection và SCRFD chạy tần suất trung bình/cao.
- Recognition chỉ chạy cho unresolved track, theo retry/backoff hiện tại.
- Known/latched track không chạy recognition lại.

### S4 — `OCCUPIED_RESOLVED`

Điều kiện: có person track và mọi eligible track hiện tại đã known hoặc đã đạt unknown policy.

- Giảm tần suất person detection để phát hiện người mới.
- SCRFD tắt khi không có track mới/unresolved.
- Recognition tắt cho resolved tracks.
- Nếu xuất hiện track mới, chuyển về `OCCUPIED_UNRESOLVED`.

### S5 — `EMPTY_GRACE`

Điều kiện: person cuối vừa biến mất nhưng chưa đủ timeout kết thúc phiên.

- Person detection chạy tần suất thấp/trung bình.
- SCRFD và recognition tắt nếu không có track.
- Person trở lại trước timeout thì giữ cùng cabin visit.
- Hết `session_empty_ms` thì về `IDLE` và kết thúc cabin visit.

### S6 — `DEGRADED`

Điều kiện: chưa có snapshot IoT, mất kết nối, state quá `state_ttl_ms`, sequence reset không hợp lệ hoặc
door/service mode là `unknown`.

- Không phụ thuộc event cửa để phát hiện người.
- Chạy person scan dự phòng.
- Có person thì cho phép SCRFD/recognition như `OCCUPIED_UNRESOLVED`.
- Chính sách gọi tầng mặc định: chỉ cho phép trong `normal`; nếu service mode không xác định thì
  suppress command và log reason, trừ khi product owner chọn fail-open riêng.

### S7 — `INHIBITED`

Điều kiện: `maintenance`, `emergency` hoặc `fire`.

- Không gọi tầng tự động.
- Mặc định giữ person detection ở tần suất thấp để audit/recording.
- Face recognition mặc định tắt; có thể bật bằng policy vận hành sau.

## 6. Lịch inference mặc định

Giả định camera nominal 30 FPS. Scheduler dùng thời gian monotonic, không phụ thuộc tuyệt đối vào số
frame; bảng FPS chỉ là giá trị cấu hình dễ hiểu.

| State | Person | SCRFD | Recognition |
|---|---:|---:|---|
| `IDLE` | 2 FPS | 0 | 0 |
| `ARMED` | 10 FPS | Khi có person, tối đa 10 FPS | Theo unresolved track |
| `DOOR_ACTIVE` | 15 FPS | Khi có person, tối đa 10 FPS | Theo unresolved track |
| `OCCUPIED_UNRESOLVED` | 10 FPS | 10 FPS | Theo retry/backoff từng track |
| `OCCUPIED_RESOLVED` | 3 FPS | Chỉ khi có track mới | 0 cho resolved track |
| `EMPTY_GRACE` | 5 FPS | 0 nếu không có track | 0 |
| `DEGRADED` | 5 FPS; tăng lên 10 khi thấy person | 10 FPS khi có person | Theo unresolved track |
| `INHIBITED` | 2 FPS | 0 mặc định | 0 mặc định |

Giá trị này là baseline rollout, phải điều chỉnh bằng benchmark trên Orange Pi. Scheduler không được
chạy nhiều hơn một NPU model cùng lúc trên context hiện tại.

## 7. Yêu cầu chức năng

### R1 — Model lifecycle

1. YOLO, SCRFD và recognition model được load một lần tại startup như hiện tại.
2. Chuyển scheduler state không được destroy/create NPU context.
3. Một model bị gate phải bỏ qua `awnn_run`, decode và preprocessing riêng không cần thiết cho model
   đó.
4. Capture vẫn phải cung cấp frame mới để scheduler có thể thức dậy nhanh.

### R2 — Person gating

1. Scheduler quyết định deadline YOLO tiếp theo bằng monotonic time.
2. Skipped YOLO frame không được gọi tracker `update` với detection rỗng.
3. Tracker `predict` và timeout vẫn tiến triển đúng khi YOLO chạy thưa.
4. Khi chuyển từ idle/degraded scan sang active, lần YOLO tiếp theo phải chạy ngay, không chờ interval
   cũ.

### R3 — Face gating

1. SCRFD không chạy ở `IDLE` nếu không có person evidence.
2. SCRFD chỉ chạy khi có track `Tracked` có detection thật, không chạy vì một `Lost/predicted_only`
   track đơn lẻ.
3. Khi bỏ qua SCRFD, `faces`, association và per-frame face label phải được clear để không dùng dữ
   liệu frame trước.
4. Orphan face fallback chỉ tồn tại khi scheduler đã cho phép face detection.

### R4 — Recognition gating

1. Recognition quyết định theo từng person track.
2. Known identity latch luôn làm `needs_recognition=false` trong vòng đời track.
3. Unknown track retry theo backoff; không chạy mỗi frame.
4. Xác nhận một resident không được tắt recognition của các track unresolved khác.
5. Track mới xuất hiện trong `OCCUPIED_RESOLVED` phải kích hoạt lại face detection và recognition.

### R5 — Elevator state validity

1. Chỉ nhận đúng `deviceCode` và elevator/slave số 2.
2. Event có sequence nhỏ hơn hoặc bằng sequence đã áp dụng bị bỏ qua, ngoại trừ snapshot reconnect
   có session/epoch mới đã được protocol định nghĩa.
3. State quá `state_ttl_ms` chuyển `DEGRADED`.
4. Dùng monotonic receive time cho TTL; không dùng đồng hồ server làm deadline nội bộ.
5. Event parser lỗi không được crash worker hoặc inference loop.

### R6 — Door behavior

1. `opening` phải đánh thức person detection ngay.
2. `open` giữ scheduler active dù frame hiện tại chưa thấy người.
3. `closing/closed` không được xóa visit hoặc track ngay; dùng person presence và empty grace.
4. Door event rung/flicker phải được debounce nhưng không trì hoãn `opening` đầu tiên.

### R7 — Fallback

1. AI Core khởi động khi IoT Core offline vẫn phát hiện người bằng `DEGRADED` scan.
2. Mất một door event không được làm pipeline tắt vô hạn.
3. Khi IoT Core reconnect, snapshot mới thay thế fallback state một cách deterministic.
4. Có CLI/config để tắt adaptive scheduling và quay về baseline nhằm rollback nhanh.

### R8 — Tích hợp gọi tầng

1. Scheduler chỉ quyết định inference; `FloorCallCoordinator` vẫn sở hữu dedupe và cabin visit.
2. `INHIBITED` phải truyền policy inhibit tới coordinator trước khi có confirmed outcome.
3. Event cửa có thể hỗ trợ cabin visit nhưng không được tự xóa dedupe khi person vẫn hiện diện.
4. Identity confirm sinh ra trong `DEGRADED` chỉ gọi tầng khi service policy cho phép.

### R9 — Realtime và thread safety

1. Socket event parsing chạy ngoài inference thread.
2. Inference loop chỉ đọc immutable snapshot hoặc atomic scheduler input.
3. Callback không được giữ socket mutex khi gọi scheduler/coordinator.
4. State transition không được block camera, NPU hoặc DB.

### R10 — Quan sát và rollback

1. Log state transition gồm old/new state và reason, không chứa tên resident hay URL camera.
2. Metrics gồm số lần YOLO/SCRFD/recognition đã chạy và đã skip theo state.
3. Báo thời gian NPU, FPS, nhiệt độ nếu nền tảng cung cấp, queue/drop và số lần fallback.
4. Shadow mode tính decision nhưng vẫn chạy baseline inference để so sánh trước khi bật gating thật.
5. Có một config duy nhất để rollback về baseline mà không build lại.

## 8. Kịch bản chấp nhận

| ID | Kịch bản | Kết quả mong đợi |
|---|---|---|
| A01 | Cửa đóng, cabin trống, state mới | `IDLE`; YOLO 2 FPS, SCRFD/recog không chạy |
| A02 | Nhận `opening` khi đang idle | Chuyển active và chạy YOLO ngay |
| A03 | Cửa mở, chưa thấy người | Person detection tiếp tục tần suất cao |
| A04 | Person xuất hiện | Bật SCRFD; recognition chỉ cho unresolved track |
| A05 | Track A đã recognized | Không recognition lại A khi quay mặt đi/quay lại |
| A06 | A resolved, B mới vào | Face/recog bật cho B, A vẫn latch |
| A07 | Hai người, A known và B unknown | Tiếp tục thử B; không tắt global recognition |
| A08 | Cửa đóng nhưng cabin còn người | Giữ occupied pipeline theo track presence |
| A09 | Person biến mất ngắn hơn empty grace | Giữ cùng cabin visit |
| A10 | Cabin trống quá empty grace | Kết thúc visit và về idle |
| A11 | IoT Core offline từ startup | `DEGRADED`; periodic scan vẫn tìm được person |
| A12 | State quá TTL | Chuyển degraded và tăng scan dự phòng |
| A13 | Event sequence cũ đến sau event mới | Bỏ event cũ, không đảo state |
| A14 | Maintenance/emergency/fire | `INHIBITED`; không gọi tầng tự động |
| A15 | Door event flicker open/closed | Không reset visit hoặc identity |
| A16 | Adaptive mode tắt | Pipeline chạy lịch baseline hiện tại |
| A17 | Shadow mode | Log decision nhưng số lần inference không thay đổi |
| A18 | Skipped SCRFD frame | Không còn face/label/association từ frame trước |

## 9. Tiêu chí hiệu năng

Không đặt phần trăm tiết kiệm trước khi đo baseline. Rollout chỉ được coi là đạt khi:

- Không tăng missed-person hoặc missed-recognition trong bộ video test đã gắn nhãn.
- Thời gian từ `doorState=opening` đến lần YOLO đầu tiên không quá 150 ms trong điều kiện app đang
  chạy bình thường.
- Thời gian từ person detection đầu tiên đến recognition attempt đầu tiên không tăng quá 200 ms so
  với baseline.
- Idle NPU executions giảm rõ rệt; mục tiêu ban đầu là giảm ít nhất 70% tổng lần `awnn_run` khi cabin
  trống.
- FPS camera/capture ổn định và không có queue tăng không giới hạn.

## 10. Ngoài phạm vi phiên bản đầu

- Unload/reload model động để giảm RAM.
- AI Core truy cập Modbus TCP/RTU trực tiếp.
- Thay đổi model hoặc quantization.
- Tối ưu lịch trình tầng theo current floor/direction.
- Dùng nhận diện khuôn mặt trong emergency/fire mode nếu chưa có yêu cầu vận hành rõ ràng.

## 11. Điểm cần chốt với IoT Core

1. Tên Socket.IO event và envelope thực tế cho elevator state.
2. Mapping raw register sang `doorState`, `serviceMode`, `moving`, `direction`.
3. Cách lấy snapshot ngay khi connect/reconnect.
4. Sequence có reset khi IoT Core restart không; nếu có cần `sessionId` hoặc `bootId`.
5. Chu kỳ heartbeat/state refresh để chọn `state_ttl_ms`.
6. Chính sách gọi tầng khi state stale: đề xuất suppress command nhưng vẫn tiếp tục recognition.
