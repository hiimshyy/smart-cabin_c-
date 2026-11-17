# Thiết kế: Nhật ký di chuyển thang máy

Tài liệu yêu cầu: [requirements.md](requirements.md)

## Overview

Thiết kế này hiện thực [requirements.md](requirements.md): ghi điểm dừng, chuyến cabin, chuyến hành khách, gọi tầng trong cabin và gọi tầng ngoài của thang 2 (`cabin_id=2`, slave `-2`, camera `cam2`) vào `db/traffic.db` trên thiết bị.

Quyết định chính:

- Trạng thái thang lấy từ thanh ghi INVT qua `CONN_DATA` của Socket.IO. Gateway chỉ gửi giá trị đổi, nên hệ thống giữ giá trị cuối trong cùng session thay cho quy tắc freshness 5 s. Mất dữ liệu được đánh dấu `register_gap`; hệ thống không suy đoán tầng.
- `ElevatorStateTracker`, `CabinTimeline`, `CallTracker` và `TripAssembler` là logic thuần chạy trên frame thread và nhận timestamp từ bên ngoài, nên test được bằng replay fixture mà không cần thread hay mạng.
- Chỉ `TrafficLogWriter` làm I/O (resolve visitor, JPEG, SQLite, purge), trên worker có queue giới hạn, bỏ bản ghi cũ nhất khi đầy.
- `OccupancyCounter` thêm `occupant_id` để Entry và Exit của cùng một người được nối với nhau qua handoff. Logic count/confidence giữ nguyên.
- Gọi tầng ngoài dùng chung cho nhóm thang. Thang 1 (slave `-1`) chỉ được theo dõi điểm dừng để biết lệnh gọi nào do thang 1 phục vụ.
- Dữ liệu nằm trong DB riêng. `residents.db` chỉ bị sửa khi `traffic_admin` nâng visitor thành resident.

Ngoài phạm vi v1: ghi DB cho thang 1, giao diện web hoặc API, gửi dữ liệu lên mạng. Các việc liên quan nhưng thuộc spec khác nằm ở mục "Ngoài phạm vi spec này" trong `tasks.md`.

## Architecture

### Luồng dữ liệu

```
SocketIoClient worker ──CONN_DATA──▶ RegisterInbox (bounded, có timestamp steady)
                                          │ drain tại frame boundary
frame loop ──────────────────────────────▼────────────────────────────────
  OccupancySnapshot (events + occupant_id)   ElevatorStateTracker (slave -2, -1)
  InteractionManager outcomes (resident)       ├─ CabinTimeline thang 2 (đầy đủ)
  face samples (embedding, chất lượng, crop)   ├─ CabinTimeline thang 1 (chỉ điểm dừng)
  FloorCall decisions (qua inbox)              └─ CallTracker (8211-2, 8215-2, 8225-2)
                         │
                         ▼
                   TripAssembler (logic thuần, không I/O, bộ nhớ có giới hạn)
                         │ CabinStopRecord / CabinRunRecord / PassengerTripRecord
                         │ CarCallRecord / HallCallRecord
                         ▼
          TrafficLogWriter worker (queue bounded, drop-oldest)
            ├─ VisitorGallery: resolve visitor
            ├─ JPEG ảnh mặt (nếu bật) + disk guard
            ├─ TrafficDB (db/traffic.db)
            └─ purge mỗi giờ
```

Mọi state machine chạy trên frame thread và nhận timestamp từ bên ngoài, nên test được mà không cần thread hay mạng. Chỉ `TrafficLogWriter` làm I/O.

### Tích hợp `main.cpp`

- Khởi tạo sau occupancy và resident DB. Cần `--traffic-db` và occupancy đã bật. Nếu Socket.IO tắt, app log warning và mọi tầng là NULL.
- Thứ tự tại frame boundary: drain register inbox → timeline → `occupancy_counter.update` → `trip_assembler.on_occupancy` → recognition (thu `FaceSample`) → interaction outcomes → `poll` → `writer.enqueue`.
- Nhánh không có frame cũng drain inbox và gọi `poll` để đóng chuyến theo timeout.
- Log sự kiện chỉ chứa ID số, ví dụ `TRIP key=... kind=visitor visitor_id=42 floor_in=1 floor_out=7 conf=high`.

## Components and Interfaces

### Bản đồ module

| Module | Vai trò |
|---|---|
| `elevator_registers.h/.cpp` | Parse `CONN_DATA` thành `RegisterUpdate` (địa chỉ → `uint16`, timestamp nhận). Decode bit cho `8480`, `8471`, `8472`, `8467` và mask gọi tầng `8211`. |
| `socketio_client` (sửa) | Tổng quát hóa `parse_socketio_door_data` thành parser nhiều thanh ghi. Thêm `RegisterCallback`. Giữ `DoorCallback` cũ cho adaptive inference. Subscribe khi bật door hoặc traffic. |
| `elevator_state.h/.cpp` | `ElevatorStateTracker` gộp cập nhật một phần theo từng slave và theo dõi session/gap. `CabinTimeline` (mỗi slave một instance) phát sự kiện `StopOpened`, `DoorOpened`, `DoorClosed`, `StopClosed`/`RunStarted`, `RunEnded`, `RegisterGap`. |
| `call_tracker.h/.cpp` | Theo dõi chuyển bit của `8211`, `8215`, `8225` trên slave `-2`, gán điểm dừng lúc đăng ký và điểm dừng phục vụ (thang 2 hoặc thang 1), khớp lệnh gọi tự động. |
| `occupancy_counter` (sửa) | Thêm `occupant_id` (lineage). Snapshot trả thêm ánh xạ track → occupant. |
| `trip_assembler.h/.cpp` | Ghép chuyến hành khách, chuyến cabin, điểm dừng, gán tầng, gom danh tính và mẫu khuôn mặt. |
| `visitor_gallery.h/.cpp` | Thư viện embedding visitor trong bộ nhớ, thuật toán resolve. |
| `traffic_db.h/.cpp` + `db/traffic_schema.sql` | Mở/migrate schema, insert, purge, export, thao tác admin. |
| `traffic_log_writer.h/.cpp` | Worker thread, queue, disk guard, JPEG, purge. |
| `traffic_admin.cpp` | Binary CLI thứ 7. |

### Thanh ghi và CabinTimeline

Địa chỉ được ghép từ `--elevator-slave` (mặc định 2): `8465-2`, `8480-2`, ... Slave của thang còn lại trong nhóm (mặc định 1) cũng được parse để theo dõi điểm dừng. `door_register` hiện có giữ nguyên để tương thích.

Quy đổi tầng: `floor_index = 8465 − 1`; bit `n` của mask gọi tầng là `floor_index = n`. Giá trị ngoài `[--traffic-floor-min, --traffic-floor-max]` bị bỏ qua và đếm vào metric `register_invalid`.

Gateway quét mỗi slave khoảng 5,6 s và chỉ gửi giá trị đổi (xem kết quả ghi mẫu trong `requirements.md`). `ElevatorStateTracker` giữ, cho từng slave: `last_value` từng thanh ghi, `last_message_ms` và `session` của socket lúc nhận. Trạng thái của slave có `valid = true` ngay khi tích lũy đủ các thanh ghi cốt lõi (`8465`, `8480`, `8211`, `8215`, `8225`, và ít nhất một trong `8471`/`8472`) trong session hiện tại (không dựa vào `"Mode": "full"`, vì snapshot đầu tiên khi subscribe có `"Mode": "new"`). Trạng thái mất hiệu lực (`RegisterGap`) khi:

- socket báo mất kết nối hoặc session tăng;
- `RegisterInbox` tràn;
- timeline đang `Running` và `now − last_message_ms > --traffic-run-timeout-ms`.

Khi đứng yên, không có message là bình thường, nên không phát gap. Khi nhận đủ bộ thanh ghi cốt lõi trở lại sau khi kết nối lại, đặt `valid = true`. Lúc đó lệnh gọi đang bật được ghi với cờ `registration_unobserved`.

Máy trạng thái `CabinTimeline`:

- `Unknown` → `Stopped(floor)` khi trạng thái `valid`, bit2 = 0 và có tầng hợp lệ.
- `Stopped` → `Running(from, dir)` khi bit2 = 1. Lúc này điểm dừng đóng và chuyến chạy mở; ghi `load_count`, `load_confidence` và mask `8211`.
- `Running` → `Stopped(to)` khi bit2 = 0. Chuyến chạy đóng và điểm dừng mới mở.
- `door_opened_at_ms` ghi nhận ở mốc thời gian đầu tiên trong điểm dừng có `(8471 & (1<<3)) != 0 || (8472 & (1<<5)) != 0`; `door_closed_at_ms` ghi nhận ở mốc đầu tiên sau `door_opened_at_ms` có `(8471 & (1<<4)) != 0 || (8472 & (1<<6)) != 0`.
- `RegisterGap` ở bất kỳ trạng thái nào → `Unknown`. Bản ghi đang mở được đóng với cờ `register_gap`, tầng chưa biết thì để NULL.
- Cờ chế độ đặc biệt được OR dần trong suốt khoảng thời gian của bản ghi.
- Giá trị thô của `8471`, `8472`, `8480` lúc tới và lúc rời được lưu vào bản ghi điểm dừng để phân tích các bit chưa có trong tài liệu (`8471` bit7, `8480` bit8/bit10).
- Thang 1 dùng cùng máy trạng thái nhưng chỉ xuất danh sách điểm dừng gần đây (vòng đệm 10 phút) cho `CallTracker`; không ghi DB trong v1.
- Khi một chuyến chạy kết thúc (`RunEnded`, bit2 chuyển `1 → 0`), timeline so bộ đếm 32-bit `(uint32(8449) << 16) | uint32(8448)` với giá trị lúc chuyến trước kết thúc (do `8448` thường tăng trễ một chu kỳ sau `RunStarted`). Chênh lệch khác 1 được log warning và cộng vào metric `run_counter_mismatch`.

### CallTracker

Đầu vào là các lần đổi giá trị của `8211-2`, `8215-2`, `8225-2` theo thứ tự nhận, cùng sự kiện timeline của hai thang và quyết định gọi tầng tự động.

- Bit 0 → 1: mở bản ghi với `registered_at`, `floor`, `kind` (`car`/`hall_up`/`hall_down`). Với lệnh gọi trong cabin, gán điểm dừng lúc đăng ký theo R14.2; gán nguồn `auto` nếu có lệnh gọi tự động tới cùng tầng đã `SENT` trong `--traffic-call-confirm-ms` trước đó. Khi khớp, quyết định gọi tầng tự động tương ứng được đánh dấu `register_confirmed`.
- Bit 1 → 0: đóng bản ghi với `cleared_at`. Chờ tối đa `--traffic-floor-tolerance-ms` (mặc định 10000 ms) để timeline xác nhận điểm dừng phục vụ tại đúng tầng có cửa mở mà `cleared_at` nằm trong `[stop.arrived_at - tol, stop.departed_at + tol]` (xét thang 2 trước, rồi tới thang 1 đối với gọi ngoài). Không có điểm dừng phù hợp → `cancelled`/`cleared_without_stop`.
- Lệnh gọi tự động `SENT` mà không có bit `8211` tương ứng trong thời gian xác nhận → quyết định được đánh dấu `register_unconfirmed`.
- Tối đa 64 bản ghi mở cho mỗi loại. Khi `RegisterGap`, mọi bản ghi mở được đóng với kết quả `register_gap`.

`RegisterInbox` là deque có giới hạn (256) bảo vệ bằng mutex. Socket worker push vào, frame thread drain ở đầu mỗi frame boundary. Khi RTSP mất hình, vòng lặp chính vẫn thức theo deadline 1 s của occupancy nên vẫn drain được. Nếu inbox tràn thì phát `RegisterGap`, vì mất một cập nhật có thể làm mất một lần chuyển trạng thái.

### Occupant lineage

`OccupancyCounter` hiện chuyển state từ track cũ sang track mới qua ghost handoff, nhưng `OccupancyEvent` chỉ mang `track_id` hiện tại. Vì vậy Entry và Exit của cùng một người có thể mang hai ID khác nhau. Thay đổi:

- `TrackState`/`Ghost` thêm `occupant_id` (`uint64`, tăng dần, 0 = không có).
- Entry hợp lệ cấp `occupant_id` mới. Handoff trực tiếp hoặc qua ghost giữ nguyên `occupant_id`.
- Exit phát `occupant_id` của slot. Slot khôi phục từ state (chưa gán) có `occupant_id = 0`, nên assembler tạo chuyến `entry_unobserved`.
- `OccupancySnapshot` thêm `std::vector<std::pair<int, uint64_t>> track_occupants` cho các track đang có slot.

Thay đổi chỉ thêm trường mới. Logic count/confidence giữ nguyên và toàn bộ test hiện có phải pass.

### TripAssembler

Đầu vào mỗi frame boundary (theo thứ tự):

1. `on_timeline_events(events)`
2. `on_occupancy(snapshot, now_ms)`
3. `on_resident_confirmed(track_id, resident_id, similarity, now_ms)`
4. `on_face_sample(track_id, sample, now_ms)`
5. `on_floor_call(decision)`, drain từ inbox vì callback chạy trên socket thread
6. `poll(now_ms)` trả các bản ghi đã hoàn tất

Trạng thái chính:

- `pending_by_track`: danh tính và mẫu khuôn mặt quan sát được trước Entry, giữ tối đa 10 s (TTL) và tối đa 32 track.
- `open_trips` theo `occupant_id`, tối đa 64. Khi đầy, chuyến cũ nhất bị đóng với cờ `exit_unobserved`.
- `track → occupant` cập nhật từ snapshot. Danh tính hoặc mẫu trên track có occupant được gắn vào chuyến của occupant đó.

Gán tầng: Entry/Exit tại thời điểm `t` được gán vào điểm dừng có `[arrived - tol, departed + tol]` chứa `t` (`tol` = `--traffic-floor-tolerance-ms`, mặc định 10000, bằng khoảng hai chu kỳ quét của gateway). Mốc `arrived`/`departed` là lúc nhận message, nên có thể trễ tới một chu kỳ so với thực tế. Nếu `t` rơi vào lúc cabin đang chạy nhưng gần một mép điểm dừng (do trễ video hoặc thanh ghi), assembler chờ timeline xác nhận điểm dừng kế tiếp tối đa `tol` rồi mới quyết định. Nếu vẫn không có điểm dừng phù hợp thì tầng để NULL và gắn `floor_unknown`. Chuyến chỉ được emit khi cả hai đầu đã được giải quyết hoặc đã hết thời gian chờ.

Danh tính lúc đóng chuyến:

- Có resident confirmed → `resident`. Nhiều resident khác nhau → chọn similarity cao nhất và gắn `identity_conflict`.
- Không có resident, có mẫu khuôn mặt và `visitor_reid` bật → giao cho writer resolve.
- Còn lại → `unidentified`.

Mẫu khuôn mặt (`FaceSample`): embedding 512 float đã chuẩn hóa L2, `quality`, `best_resident_id`, `best_resident_sim`, và crop 112×112 nếu bật lưu ảnh. `quality = det_score × min(1, face_width / 80 px)`. Mỗi chuyến giữ top-K theo `quality`. Crop chỉ được clone khi tốt hơn crop đang giữ.

Để có `best_resident_id` khi dưới ngưỡng, `main.cpp` gọi `match_engine.match(emb, -1.0f)` rồi tự so với `eff.match_thr`. Số phép tính không đổi.

Mỗi chuyến cabin có danh sách `trip_key` của các chuyến đang mở lúc rời điểm dừng. Writer ghi danh sách này vào bảng `run_passengers`.

Khóa bản ghi: `<session_hex>-<seq>`, trong đó `session_hex` là số ngẫu nhiên 64 bit sinh lúc app khởi động. Khóa duy nhất qua các lần restart nên các bảng có thể tham chiếu nhau trước khi được insert.

### VisitorGallery

- Nạp mọi embedding visitor có `status='active'` lúc khởi động. Lưu dạng flat float giống `MatchEngine`.
- Thuật toán resolve:
  1. Loại outlier: tính centroid của các mẫu, bỏ mẫu có cos với centroid < 0.5. Nếu sau khi loại còn dưới 2 mẫu thì resolve không có visitor nào và chuyến ghi `unidentified`.
  2. So centroid với mọi embedding visitor bằng max-cos theo visitor.
  3. Gán nếu `best ≥ visitor_match_thr` (mặc định 0.45, sẽ hiệu chỉnh) và `best - runner_up ≥ 0.05`.
  4. Nếu không gán được thì tạo visitor mới.
- Cập nhật: thêm mẫu tốt nhất của chuyến vào visitor, tối đa M = 10 mẫu, thay mẫu `quality` thấp nhất. Cập nhật `last_seen_ms` và `trip_count`.
- Ngưỡng 0.45 chỉ là giá trị khởi đầu. Task hiệu chỉnh đo tỉ lệ gộp nhầm và tách nhầm trên các recording đã có, rồi chốt giá trị.
- Với khoảng 100 visitor mới mỗi ngày, 10 mẫu mỗi visitor và hạn 90 ngày, thư viện tối đa khoảng 90.000 vector. Một lần resolve quét khoảng 46 triệu phép nhân-cộng, mất vài chục ms trên CPU A733. Việc này chạy trên worker nên không ảnh hưởng FPS.

### TrafficLogWriter

- Queue dạng `std::deque`, tối đa 1024 bản ghi, bỏ bản ghi cũ nhất khi đầy, đếm `dropped`.
- Thứ tự xử lý: resolve visitor → ghi JPEG (`<db_dir>/traffic_faces/YYYY-MM-DD/<trip_key>.jpg`, quyền `0600`, nếu bật và disk guard cho phép) → insert trong một transaction cho mỗi lô ≤ 50 bản ghi.
- Disk guard dùng `statvfs` trên thư mục DB và thư mục ảnh, kiểm tra mỗi 60 s.
- Purge mỗi giờ: xóa bản ghi quá hạn theo lô 500 dòng mỗi transaction; chuyển visitor quá hạn sang `expired` và xóa embedding; xóa ảnh quá hạn hoặc vượt tổng dung lượng, cũ nhất trước.
- `close()`: nhận các bản ghi `shutdown` từ assembler, flush, rồi join.
- Metrics đưa vào `system_metrics`: `traffic_q`, `traffic_drop`, `open_trips`, `open_calls`, `reg_age_ms`, `reg_inv`, `run_mismatch`, `visitors`, `traffic_err`.

## Data Models

Các bản ghi `TripAssembler` phát ra (`CabinStopRecord`, `CabinRunRecord`, `PassengerTripRecord`, `CarCallRecord`, `HallCallRecord`) được writer ghi vào các bảng `cabin_stops`, `cabin_runs`, `passenger_trips`, `car_calls`, `hall_calls`. Danh sách `trip_key` của chuyến cabin được ghi vào `run_passengers`.

### Schema `db/traffic.db`

```sql
CREATE TABLE app_sessions (
    session_key TEXT PRIMARY KEY, started_at_ms INTEGER NOT NULL,
    cabin_id INTEGER NOT NULL, elevator_slave INTEGER NOT NULL);

CREATE TABLE cabin_stops (
    stop_key TEXT PRIMARY KEY, cabin_id INTEGER NOT NULL, floor INTEGER, height_cm INTEGER,
    arrived_at_ms INTEGER, door_opened_at_ms INTEGER, door_closed_at_ms INTEGER,
    departed_at_ms INTEGER, boarded INTEGER NOT NULL DEFAULT 0,
    alighted INTEGER NOT NULL DEFAULT 0,
    raw_8471_arrive INTEGER, raw_8472_arrive INTEGER, raw_8480_arrive INTEGER,
    raw_8471_depart INTEGER, raw_8472_depart INTEGER, raw_8480_depart INTEGER,
    flags INTEGER NOT NULL DEFAULT 0);

CREATE TABLE cabin_runs (
    run_key TEXT PRIMARY KEY, cabin_id INTEGER NOT NULL,
    from_stop_key TEXT, to_stop_key TEXT, from_floor INTEGER, to_floor INTEGER,
    direction TEXT NOT NULL CHECK (direction IN ('up','down','unknown')),
    departed_at_ms INTEGER, arrived_at_ms INTEGER,
    load_count INTEGER, load_confidence TEXT CHECK (load_confidence IN ('certain','uncertain')),
    car_calls_mask INTEGER, flags INTEGER NOT NULL DEFAULT 0);

CREATE TABLE passenger_trips (
    trip_key TEXT PRIMARY KEY, cabin_id INTEGER NOT NULL,
    entered_at_ms INTEGER, exited_at_ms INTEGER, floor_in INTEGER, floor_out INTEGER,
    identity_kind TEXT NOT NULL CHECK (identity_kind IN ('resident','visitor','unidentified')),
    resident_id INTEGER, visitor_id INTEGER, identity_sim REAL,
    suggested_resident_id INTEGER, suggested_resident_sim REAL,
    called_floor INTEGER, call_state TEXT,
    call_register_confirmed INTEGER CHECK (call_register_confirmed IN (0,1)),
    car_calls_mask INTEGER, rides INTEGER NOT NULL DEFAULT 0,
    boarded_stop_key TEXT, alighted_stop_key TEXT,
    hall_call_key TEXT, car_call_key TEXT,
    confidence TEXT NOT NULL CHECK (confidence IN ('high','low')),
    flags INTEGER NOT NULL DEFAULT 0, face_image TEXT);

CREATE TABLE car_calls (
    call_key TEXT PRIMARY KEY, cabin_id INTEGER NOT NULL, floor INTEGER NOT NULL,
    registered_at_ms INTEGER, registered_stop_key TEXT, origin_floor INTEGER,
    cleared_at_ms INTEGER, served_stop_key TEXT,
    outcome TEXT CHECK (outcome IN ('served','cancelled','register_gap','shutdown')),
    source TEXT NOT NULL CHECK (source IN ('auto','button')),
    flags INTEGER NOT NULL DEFAULT 0);

CREATE TABLE hall_calls (
    call_key TEXT PRIMARY KEY, floor INTEGER NOT NULL,
    direction TEXT NOT NULL CHECK (direction IN ('up','down')),
    registered_at_ms INTEGER, cleared_at_ms INTEGER,
    served_cabin_id INTEGER, served_stop_key TEXT, door_opened_at_ms INTEGER, wait_ms INTEGER,
    outcome TEXT CHECK (outcome IN ('served','served_by_car1','cleared_without_stop',
                                    'register_gap','shutdown')),
    flags INTEGER NOT NULL DEFAULT 0);

CREATE TABLE run_passengers (
    run_key TEXT NOT NULL, trip_key TEXT NOT NULL, PRIMARY KEY (run_key, trip_key));

CREATE TABLE visitors (
    id INTEGER PRIMARY KEY AUTOINCREMENT, first_seen_ms INTEGER NOT NULL,
    last_seen_ms INTEGER NOT NULL, trip_count INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'active'
        CHECK (status IN ('active','expired','merged','promoted','deleted')),
    merged_into INTEGER, promoted_resident_id INTEGER);

CREATE TABLE visitor_embeddings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    visitor_id INTEGER NOT NULL REFERENCES visitors(id) ON DELETE CASCADE,
    vector BLOB NOT NULL, quality REAL NOT NULL, created_at_ms INTEGER NOT NULL);

CREATE TABLE admin_audit (
    id INTEGER PRIMARY KEY AUTOINCREMENT, at_ms INTEGER NOT NULL, action TEXT NOT NULL,
    visitor_id INTEGER, resident_id INTEGER, detail TEXT);
```

Index: `passenger_trips(entered_at_ms)`, `(exited_at_ms)`, `(visitor_id)`, `(resident_id)`, `(floor_in, floor_out)`; `cabin_runs(departed_at_ms)`; `cabin_stops(arrived_at_ms)`; `car_calls(registered_at_ms)`, `(origin_floor, floor)`; `hall_calls(registered_at_ms)`, `(floor, direction)`; `visitor_embeddings(visitor_id)`.

`floor` trong mọi bảng là chỉ số tầng (B = 0). Export CSV thêm cột nhãn tầng (`B`, `1`, …, `9`).

### Cờ `flags`

Bitmask, dùng chung cho mọi bảng:

| Bit | Tên |
|---|---|
| 0 | `occupancy_uncertain_in` |
| 1 | `occupancy_uncertain_out` |
| 2 | `entry_unobserved` |
| 3 | `exit_unobserved` |
| 4 | `floor_unknown` |
| 5 | `register_gap` |
| 6 | `no_ride` |
| 7 | `identity_conflict` |
| 8 | `shutdown` |
| 9 | `maintenance` |
| 10 | `fault` |
| 11 | `fire` |
| 12 | `independent` |
| 13 | `parking` |
| 14 | `overload` |
| 15 | `load_implausible` |
| 16 | `registered_while_running` |
| 17 | `registration_unobserved` |

`confidence='high'` chỉ khi không có bit 0–5.

### Thời gian

Thời gian lưu dạng UTC epoch ms. Logic nội bộ dùng steady clock. Khi enqueue, mỗi bản ghi được chuyển sang wall clock bằng offset `system_now - steady_now` lấy ngay lúc đó, nên NTP chỉnh giờ chỉ ảnh hưởng các bản ghi sau. `residents.db` không bị sửa, trừ thao tác promote của `traffic_admin`.

### Ước lượng dung lượng và thời hạn giữ (thẻ 64 GB)

Số liệu thực tế từ log cam2 ngày 28/10 – 03/11: 70–238 Entry mỗi ngày. Thẻ nhớ hiện còn trống 22 GB trên tổng 58 GB.

| Dữ liệu | Kích thước | Mỗi ngày (giả định trên: 1.000 chuyến) | Với thời hạn mặc định |
|---|---|---|---|
| Chuyến hành khách + chuyến cabin + điểm dừng + index | ≈ 1 KB/chuyến | ≈ 1 MB | 730 ngày ≈ 0,7 GB |
| Embedding visitor | 2 KB/vector, ≤ 10/visitor | ≤ 2 MB | 90 ngày ≈ 0,2 GB |
| Ảnh mặt JPEG 112×112 | 4–6 KB | ≈ 5 MB | 30 ngày ≈ 0,15 GB (giới hạn 5 GB) |

Dung lượng không phải giới hạn. Rủi ro chính là thẻ nhớ hỏng và mất toàn bộ dữ liệu. Vì vậy cần export CSV định kỳ và sao lưu `traffic.db` bằng `sqlite3 .backup` sang USB hoặc máy khác (lệnh vận hành, ghi trong README).

## Correctness Properties

*Property là đặc tính phải đúng trên mọi lần chạy hợp lệ của hệ thống, tức một phát biểu hình thức về việc hệ thống phải làm gì. Property là cầu nối giữa đặc tả đọc được và kiểm chứng tự động.*

Các Property 1 đến 17 áp dụng cho logic thuần (`elevator_registers`, `ElevatorStateTracker`, `CabinTimeline`, `CallTracker`, `OccupancyCounter`, `TripAssembler`, `VisitorGallery`), nên test được trên host bằng timestamp giả và replay fixture. Property 18 đến 20 liên quan tới `TrafficLogWriter` và frame loop trong `src/main.cpp`. Trong dòng **Validates**, `RN.M` là mục M của Requirement RN trong requirements.md.

### Property 1: Parse thanh ghi

*Với mọi* message `CONN_DATA`, parser chỉ nhận message đúng device code. Giá trị được nhận phải là số nguyên trong 0..65535, kể cả khi gửi dạng chuỗi số; giá trị sai bị bỏ qua. Message chỉ chứa một phần thanh ghi chỉ cập nhật các thanh ghi có mặt.

**Validates: Requirements 1.1, 1.5**

### Property 2: Quy đổi tầng

*Với mọi* giá trị `8465` và mọi mask `8211`/`8215`/`8225`, chỉ số tầng bằng `8465 − 1`, và bit `n` của mask ứng với tầng `n`. Chỉ số ngoài `[--traffic-floor-min, --traffic-floor-max]` không xuất hiện trong bản ghi nào và làm tăng `register_invalid`.

**Validates: Requirements 1.1, 1.5**

### Property 3: Giá trị cuối trong cùng session

*Với mọi* chuỗi message trong cùng một session socket, giá trị của mỗi thanh ghi trong `ElevatorStateTracker` bằng giá trị nhận gần nhất của thanh ghi đó trên đúng slave. Khi timeline đang `Stopped`, khoảng lặng dài bao nhiêu cũng không phát `RegisterGap`.

**Validates: Requirements 1.2**

### Property 4: Điều kiện mất dữ liệu

*Với mọi* chuỗi sự kiện socket, `RegisterGap` được phát khi và chỉ khi xảy ra một trong ba điều kiện: mất kết nối hoặc session tăng; `RegisterInbox` tràn; timeline đang `Running` và `now − last_message_ms > --traffic-run-timeout-ms`. Sau gap, timeline ở `Unknown` cho tới khi slave nhận đủ các thanh ghi cốt lõi trong session hiện tại. Mọi bản ghi đang mở lúc gap được đóng với cờ hoặc kết quả `register_gap`, và tầng chưa biết để NULL.

**Validates: Requirements 1.2, 2.5, 13.2, 14.2**

### Property 5: Điểm dừng, chuyến chạy và giờ cửa

*Với mọi* chuỗi giá trị thanh ghi không có gap, timeline của mỗi slave phát điểm dừng và chuyến chạy xen kẽ nhau. Mỗi chuyến chạy mở đúng lúc điểm dừng trước đóng (bit2 của `8480` chuyển 0 → 1) và đóng đúng lúc điểm dừng sau mở (bit2 chuyển 1 → 0); `from_floor` và `to_floor` bằng tầng của hai điểm dừng đó. `door_opened_at_ms` là mốc đầu tiên trong điểm dừng có `8471` bit3 hoặc `8472` bit5 bật; `door_closed_at_ms` là mốc đầu tiên sau `door_opened_at_ms` có `8471` bit4 hoặc `8472` bit6 bật.

**Validates: Requirements 2.1, 2.2, 2.3**

### Property 6: Cờ chế độ đặc biệt

*Với mọi* bản ghi điểm dừng hoặc chuyến chạy, `flags` chứa bit của một chế độ đặc biệt (bảo trì, lỗi, báo cháy, độc lập, khóa, quá tải) khi và chỉ khi chế độ đó được quan sát ít nhất một lần trong khoảng thời gian của bản ghi.

**Validates: Requirements 2.4**

### Property 7: Đối chiếu bộ đếm chuyến chạy

*Với mọi* chuyến chạy kết thúc (`RunEnded`), nếu chênh lệch bộ đếm 32-bit `(uint32(8449) << 16) | uint32(8448)` so với lúc chuyến trước kết thúc khác 1 thì timeline log warning và tăng `run_counter_mismatch` đúng một lần. Replay fixture `tests/fixtures/conn_data_20261103.jsonl` cho 12 chuyến chạy của thang 2, khớp chênh lệch 12 của `8448`.

**Validates: Requirements 2.6**

### Property 8: Vòng đời lệnh gọi

*Với mọi* chuỗi giá trị `8211-2`, `8215-2`, `8225-2`, mỗi lần bit chuyển 0 → 1 mở đúng một bản ghi, và lần chuyển 1 → 0 kế tiếp của cùng bit đóng bản ghi đó. Mỗi bản ghi kết thúc với đúng một kết quả. Số bản ghi mở của mỗi loại không vượt 64. Lệnh gọi đã bật sẵn ở snapshot đầu tiên hoặc sau gap có cờ `registration_unobserved` và không được dùng để tính thời gian chờ.

**Validates: Requirements 13.1, 13.3, 14.1**

### *Property 9: Kết quả gọi tầng ngoài

*Với mọi* lệnh gọi ngoài bị xóa, kết quả là `served` nếu thang 2 có điểm dừng ở đúng tầng và mở cửa sao cho `cleared_at` nằm trong `[stop.arrived_at - tol, stop.departed_at + tol]`; nếu không thì `served_by_car1` nếu thang 1 có điểm dừng tương tự ở tầng đó; nếu không nữa thì `cleared_without_stop`. Với kết quả `served`, `wait_ms = door_opened_at_ms − registered_at_ms`.

**Validates: Requirements R13.2**

### *Property 10: Lệnh gọi trong cabin

*Với mọi* lệnh gọi trong cabin, điểm dừng lúc đăng ký là điểm dừng có `[arrived, departed + tol]` chứa giờ đăng ký; nếu lệnh được đăng ký lúc cabin đang chạy thì dùng điểm dừng trước đó và gắn `registered_while_running`. Kết quả là `served` khi thang 2 dừng ở tầng đích và mở cửa, ngược lại là `cancelled`. Nguồn là `auto` khi và chỉ khi có lệnh gọi tự động tới cùng tầng đã `SENT` trong `--traffic-call-confirm-ms` trước giờ đăng ký; ngược lại là `button`.

**Validates: Requirements R14.2**

### *Property 11: Xác nhận lệnh gọi tự động

*Với mọi* quyết định gọi tầng tự động ở trạng thái `SENT`, quyết định được đánh dấu `register_confirmed` khi và chỉ khi bit `8211` của tầng đó chuyển 0 → 1 trong `--traffic-call-confirm-ms`; ngược lại là `register_unconfirmed`. Định dạng và hành vi của lệnh gọi `8193-2` không đổi.

**Validates: Requirements R1.6, R4.4**

### *Property 12: Occupant lineage

*Với mọi* chuỗi frame, Entry hợp lệ cấp `occupant_id` mới (tăng dần, khác 0). Handoff trực tiếp hoặc qua ghost giữ nguyên `occupant_id`. Exit mang `occupant_id` của slot; slot khôi phục từ state có `occupant_id = 0`. Với cùng đầu vào, count và confidence giống hệt trước khi thêm lineage.

**Validates: Requirements R3.1, R3.4**

### *Property 13: Ghép chuyến hành khách

*Với mọi* chuỗi sự kiện occupancy, mỗi `occupant_id` khác 0 có tối đa một chuyến mở, và mỗi chuyến được emit đúng một lần. Exit có `occupant_id = 0` tạo chuyến chỉ có đầu ra với cờ `entry_unobserved`. Chuyến không có Exit được đóng với cờ `exit_unobserved` khi occupancy reconcile về 0, khi app tắt, khi quá `max_trip_ms`, hoặc khi `open_trips` đầy (tối đa 64) thì chuyến cũ nhất bị đóng. Vào rồi ra ở cùng tầng mà cabin không chạy được gắn `no_ride`.

**Validates: Requirements R3.1, R3.4, R3.5, R3.6**

### *Property 14: Gán tầng

*Với mọi* Entry hoặc Exit tại thời điểm `t`, tầng được gán là tầng của điểm dừng có `[arrived − tol, departed + tol]` chứa `t`. Nếu không có điểm dừng như vậy sau khi chờ tối đa `tol`, tầng là NULL và chuyến có cờ `floor_unknown`. Chuyến chỉ được emit khi cả hai đầu đã giải quyết hoặc đã hết thời gian chờ.

**Validates: Requirements R3.2, R11.3**

### *Property 15: Danh tính

*Với mọi* chuyến, `identity_kind = 'resident'` khi lineage có ít nhất một outcome confirmed, kể cả outcome trong 10 s trước Entry. Nếu có nhiều resident khác nhau, chuyến lấy resident có similarity cao nhất và gắn `identity_conflict`. Không có resident thì chuyến chỉ có thể là `visitor` khi `--traffic-visitor-reid` bật và có mẫu khuôn mặt; còn lại là `unidentified`. Mỗi chuyến giữ tối đa K mẫu có `quality` cao nhất.

**Validates: Requirements R4.1, R4.2, R5.1, R5.7**

### *Property 16: Resolve visitor

*Với mọi* tập mẫu và thư viện visitor, sau khi bỏ mẫu có cos với centroid < 0.5: nếu còn dưới 2 mẫu thì chuyến là `unidentified`; chuyến được gán visitor cũ khi và chỉ khi `best ≥ visitor_match_thr` và `best − runner_up ≥ 0.05`; còn lại tạo visitor mới. Sau cập nhật, mỗi visitor có tối đa M = 10 mẫu, và mẫu bị thay là mẫu có `quality` thấp nhất.

**Validates: Requirements R5.2, R5.3**

### *Property 17: Mức tin cậy

*Với mọi* chuyến hành khách, `confidence = 'high'` khi và chỉ khi `flags` không có bit nào trong 0–5.

**Validates: Requirements R3.3, R3.4, R3.7**

### *Property 18: Frame loop không bị chặn

*Với mọi* mức tải, frame thread không làm I/O đĩa hay mạng cho traffic. `RegisterInbox` chứa tối đa 256 phần tử, queue của writer tối đa 1024 bản ghi; khi queue đầy, bản ghi cũ nhất bị bỏ và `traffic_dropped` tăng. Resolve visitor, mã hóa JPEG, insert và purge chỉ chạy trên worker.

**Validates: Requirements R5.6, R9.1**

### *Property 19: Tắt app

*Với mọi* trạng thái lúc tắt, mọi chuyến và lệnh gọi đang mở được đóng với cờ hoặc kết quả `shutdown` và được enqueue trước khi writer flush. Worker chỉ được join sau khi flush xong.

**Validates: Requirements R9.2**

### *Property 20: Log không chứa PII

*Với mọi* log của các module traffic, nội dung chỉ gồm ID số (`trip_key`, `resident_id`, `visitor_id`), tầng, cờ và số đếm; không có tên, `greeting_name`, căn hộ, embedding hay đường dẫn ảnh.

**Validates: Requirements R10.1** (và hard rule về PII trong `AGENTS.md`)

## Error Handling

Nguyên tắc: lỗi ở dữ liệu thanh ghi, occupancy hay lưu trữ không được dừng nhận diện hay chặn frame loop. Khi không tin được dữ liệu, bản ghi vẫn được lưu, trường chưa biết để NULL và có cờ lý do; hệ thống không suy đoán tầng (R2.5).

| Tình huống | Xử lý |
|---|---|
| `CONN_DATA` sai device code | Bỏ message |
| Giá trị thanh ghi không phải số nguyên 16 bit | Bỏ qua giá trị; log cảnh báo có giới hạn tần suất (R1.5) |
| Tầng ngoài `[--traffic-floor-min, --traffic-floor-max]` | Bỏ qua, tăng `register_invalid` |
| Thang đứng yên, lâu không có message | Bình thường: giữ giá trị cuối, không phát gap |
| Socket mất kết nối hoặc session tăng | `RegisterGap`: timeline về `Unknown`; điểm dừng/chuyến chạy đang mở đóng với cờ `register_gap`, tầng chưa biết để NULL; lệnh gọi đang mở đóng với kết quả `register_gap` |
| `RegisterInbox` tràn (256) | `RegisterGap` như trên |
| Cabin đang chạy, không có message của slave quá `--traffic-run-timeout-ms` | `RegisterGap` như trên |
| Snapshot đầy đủ đầu tiên sau khi kết nối lại | `valid = true`; lệnh gọi đang bật được ghi với `registration_unobserved`, không tính thời gian chờ |
| Chênh lệch `8448`/`8449` khác 1 | Log warning, tăng `run_counter_mismatch`; bản ghi giữ nguyên |
| Socket.IO tắt hoặc không có dữ liệu thanh ghi | Log warning lúc khởi động; chuyến hành khách vẫn được ghi với tầng NULL và cờ `floor_unknown` (R11.3) |
| Thiếu `--traffic-db` hoặc occupancy chưa bật | Không khởi tạo tính năng traffic |
| Entry/Exit không nằm trong điểm dừng nào | Chờ tối đa `tol`; vẫn không có thì tầng NULL, cờ `floor_unknown` |
| Entry/Exit nằm trong cửa sổ dung sai của hai điểm dừng liền nhau | Ưu tiên điểm dừng có `[arrived_at, departed_at]` chứa `t`; nếu không có, chọn điểm dừng có khoảng cách thời gian tới `[arrived_at, departed_at]` nhỏ hơn (nếu bằng nhau: `Entry` chọn điểm dừng trước chuyến chạy, `Exit` chọn điểm dừng sau chuyến chạy) |
| Exit có `occupant_id = 0` | Tạo chuyến chỉ có đầu ra, cờ `entry_unobserved` |
| Entry không có Exit | Đóng khi occupancy reconcile về 0, khi tắt app, hoặc sau `max_trip_ms`; cờ `exit_unobserved` |
| `open_trips` đầy (64) | Đóng chuyến cũ nhất với cờ `exit_unobserved` |
| Count lúc rời điểm dừng lớn hơn `--traffic-cabin-capacity` | `load_count` NULL, cờ `load_implausible` (R2.2) |
| Một lineage có nhiều resident | Chọn similarity cao nhất, cờ `identity_conflict` |
| Còn dưới 2 mẫu sau khi loại outlier | Chuyến ghi `unidentified` |
| Queue writer đầy (1024) | Bỏ bản ghi cũ nhất, tăng `traffic_dropped` |
| Dung lượng trống dưới `--traffic-min-free-mb` (mặc định 4096) | Ngừng lưu ảnh (R8.3) |
| Dung lượng trống dưới mức tới hạn (mặc định 1024 MB) | Ngừng ghi bản ghi mới và log error (R8.3) |
| NTP chỉnh giờ | Chỉ ảnh hưởng các bản ghi enqueue sau đó, vì offset wall clock lấy lúc enqueue |
| Tắt app (`SIGTERM`) | Đóng mọi bản ghi đang mở với `shutdown`, flush queue, rồi join worker (R9.2) |
| Mở hoặc migrate `db/traffic.db` thất bại; insert hoặc purge lỗi | Mở/migrate lỗi lúc khởi động: log `ERROR` và tắt riêng module traffic (app chính vẫn chạy); `insert`/`purge` lỗi lúc chạy: rollback transaction lô hiện tại, log `WARN` có rate-limit (5 s) và tăng biến đếm lỗi trong `system_metrics` |

## Testing Strategy

Test chạy bằng `bash tests/run_tests.sh`; mỗi test là một `main()` riêng và phải được đăng ký trong script. Số trong ngoặc là property mà test kiểm.

### Unit tests

- `test_elevator_registers`: parse nhiều thanh ghi, cập nhật một phần, chuỗi số, giá trị vượt 16 bit, sai device, decode bit (Property 1, 2).
- `test_elevator_state`: dừng → chạy → dừng, cửa theo `8471` và fallback `8472`, đứng yên lâu không có message (không phải gap), chạy quá `run_timeout` (gap), đổi session, inbox tràn, chế độ đặc biệt, so khớp `8448`. Replay fixture `tests/fixtures/conn_data_20261103.jsonl` (bản ghi mẫu 10 phút, chỉ chứa giá trị thanh ghi và device code) phải cho đúng 12 chuyến chạy của thang 2 và chuỗi điểm dừng `1,5,6,7,8,3,8,1,3,5,1,6,5` (chỉ số tầng) (Property 3 đến 7).
- `test_call_tracker`: replay cùng fixture phải cho 8 lệnh gọi trong cabin `served` của thang 2, 5 gọi ngoài `served` và 6 gọi ngoài `served_by_car1`; cộng các ca tổng hợp cho `cancelled`, `registered_while_running`, `registration_unobserved`, xác nhận lệnh gọi tự động (Property 8 đến 11).
- `test_occupancy_counter` (bổ sung): `occupant_id` qua handoff trực tiếp và qua ghost; slot khôi phục cho `occupant_id = 0`; count/confidence không đổi (Property 12).
- `test_trip_assembler`: chuyến đủ hai đầu; thiếu Entry; thiếu Exit (timeout, reconcile, shutdown); `no_ride`; trễ ±tol; danh tính trước Entry; `identity_conflict`; giới hạn `open_trips` (Property 13, 14, 15).
- `test_visitor_gallery`: khớp, tạo mới, margin, loại outlier, thay mẫu khi đầy (Property 16).
- `test_traffic_db` (SQLite, nhận `db/traffic_schema.sql` qua `argv[1]`): schema, insert, purge, export, promote/merge/delete và audit.
- `test_traffic_log_writer`: tràn queue, disk guard (inject `statvfs`), flush khi tắt (Property 18 phần queue, Property 19).
- `test_app_config`, `test_env_face_cabin.sh`: tham số mới.

Property 17, Property 18 (phần frame thread không làm I/O) và Property 20 chưa có test riêng trong danh sách trên; kiểm bằng review code và log khi live test.

### Kiểm chứng trên thiết bị

Các bước này nằm ở giai đoạn 0 và 7 của `tasks.md`:

- Ghi mẫu `CONN_DATA` dài hơn (giai đoạn 0b) để quan sát một lệnh gọi tự động `8193-2` làm bật bit `8211` tương ứng (Property 11).
- Hiệu chỉnh `visitor_match_thr` trên các recording đã có, đo tỉ lệ gộp nhầm và tách nhầm (Property 16).
- Live test 1 ngày: đối chiếu số chuyến cabin với chênh lệch `8448`/`8449` (Property 7), kiểm tầng vào/ra thủ công trên ít nhất 20 chuyến hành khách (Property 14), kiểm thời gian chờ trên ít nhất 10 lệnh gọi ngoài (Property 9).

## Rủi ro đã biết

- Bộ đếm người production bỏ sót phần lớn lượt vào/ra và đang ở count = 275. Trong phiên ghi mẫu 10 phút: 12 chuyến chạy, 8 lệnh gọi trong cabin của thang 2, nhưng chỉ 2 Entry và 2 Exit. Theo log, 29/10 có 238 Entry và 167 Exit. Vì vậy dữ liệu chuyến hành khách gắn danh tính sẽ thiếu nhiều cho tới khi `cabin-occupancy` được sửa. Dữ liệu OD theo điểm dừng (R14) và thời gian chờ (R13) không phụ thuộc vào vấn đề này.
- Mọi mốc thời gian lấy từ thanh ghi có sai số tới một chu kỳ quét của gateway (khoảng 5,6 s). Nếu gateway quét nhanh hơn được thì nên giảm chu kỳ; thiết kế không phụ thuộc vào giá trị cụ thể.
- Gọi ngoài dùng chung cho nhóm thang. Phân bổ cho thang 1 dựa vào điểm dừng của thang 1, không có camera xác nhận.
- Trong 10 phút ghi mẫu không có lệnh gọi tự động, nên quy ước bit của `8193-2` mới được suy ra từ code (`2^n`) chứ chưa quan sát trên thanh ghi.
- Ngưỡng visitor chưa hiệu chỉnh. Gộp nhầm hai người là lỗi khó phát hiện hơn tách nhầm, nên ngưỡng khởi đầu được đặt cao.
