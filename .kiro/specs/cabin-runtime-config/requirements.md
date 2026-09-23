# Requirements — Lớp config vận hành DB-driven (web/app chỉnh RTSP + settings cabin)

**Spec ID**: `cabin-runtime-config` · **Đề xuất số**: 4 (config vận hành)
**Phụ thuộc**: `resident-db-layer` (ResidentDB, `db/schema.sql`, MatchEngine, InteractionManager),
`main.cpp` (CLI args + `CamConfig` + `capture_worker`), spec Enroll API (đội web gọi vào — cùng REST layer)
**Liên quan**: `DEVELOPMENT_PLAN.md` §3.2 (ghi chú config), §3.4 (Enroll API), §2.1 (topology web-on-Pi)

## 1. Bối cảnh & vấn đề

Hiện `face_recog_app` cấu hình **hoàn toàn bằng CLI args + env** khi khởi động
(`--source`, `--gst-latency`, `--cabin-id`, `--match-thr`, `--confirm-streak`,
`--cooldown-ms`, `--unknown-after-ms`, `--reconnect-min/max-ms`, ...). Đọc 1 lần lúc start,
sau đó bất biến. Muốn đổi bất kỳ tham số nào phải sửa lệnh khởi động rồi restart bằng tay.

Yêu cầu vận hành mới: **web/app phải đổi được cấu hình từng cabin từ xa**, quan trọng nhất là
**link RTSP camera** (mỗi cabin một camera, IP đổi khi thay thiết bị), mà không cần lập trình viên
SSH vào Pi sửa lệnh.

Vấn đề bản chất:

- CLI args không phải kênh mà web ghi vào được. Web ghi được vào **SQLite** (cùng DB đang dùng cho
  residents), qua REST API đã có kế hoạch.
- `db/schema.sql` **đã có sẵn bảng `cabins`** với `camera_urls` (JSON array RTSP),
  `elevator_endpoint`, `floors_min/max` — nhưng **chưa có code C++ nào đọc bảng này**. `main.cpp`
  lấy RTSP từ `--source`, không từ `cabins.camera_urls`.
- Đây là một quyết định kiến trúc: **nguồn chân lý của config vận hành chuyển từ CLI sang DB**.
  CLI vẫn giữ để dev/test override, nhưng khi chạy thật app đọc config từ bảng `cabins`.

Spec này chốt: (a) những setting nào web được chỉnh, (b) chúng lưu ở đâu, (c) app đọc + áp dụng
thế nào, (d) đổi RTSP khi đang chạy 24/7 xử lý ra sao.

## 2. Phạm vi

**Trong scope:**

1. Migration `schema_version = 2`: thêm các cột config vận hành vào bảng `cabins`
   (đồng bộ với cột `residents.ext_id` mà `DEVELOPMENT_PLAN.md` §3.5 dự kiến — gộp cùng version 2).
2. `main.cpp` (qua một struct `CabinConfig`) đọc config vận hành từ bảng `cabins` theo `--cabin-id`
   lúc khởi động, thay vì chỉ từ CLI.
3. Quy ước ưu tiên (precedence) **CLI > DB > default** cho mọi tham số vận hành.
4. Phân loại 3 nhóm setting: web-chỉnh-được / admin-only / không-cho-web-đụng.
5. Chiến lược đổi RTSP (và mọi config vận hành) khi app đang chạy: **ghi DB → restart service**
   (Cách B), không hot-reload ở v1.
6. Định nghĩa các trường config để lớp REST API (spec Enroll API) đọc/ghi qua
   `GET/PATCH /api/v1/cabins/{id}`.

**Ngoài scope:**

- Hot-reload camera không gián đoạn (đổi RTSP mà app không restart) — để dành nếu thực sự cần.
- Bản thân REST API endpoints (thuộc spec Enroll API); spec này chỉ định nghĩa **dữ liệu** mà API đó
  đọc/ghi và ràng buộc validate.
- TTS on/off, ngôn ngữ mặc định cabin, ngưỡng liveness — nêu tên như "config tương lai" nhưng chưa
  triển khai (phụ thuộc Giai đoạn 2/3 chưa có code).
- File `config.yaml` — spec này thay thế hẳn ý tưởng YAML ở `DEVELOPMENT_PLAN.md` §3.2 bằng DB-driven.

## 3. Phân loại setting (quyết định trung tâm của spec)

Phân theo **ai được chỉnh**, vì không phải tham số nào cũng an toàn khi cho web đụng vào.

### Nhóm A — Per-cabin, web/app CHỈNH ĐƯỢC (config vận hành)

Lưu trong bảng `cabins`, một dòng / cabin. Web đọc/ghi qua REST API.

| Setting | CLI hiện tại | Cột `cabins` | Ghi chú |
|---|---|---|---|
| RTSP camera URL | `--source` | `camera_urls` (đã có) | JSON array; v1 dùng phần tử [0] |
| RTSP jitter latency | `--gst-latency` | `gst_latency_ms` (thêm) | mặc định 100 |
| Cabin id | `--cabin-id` | `id` (PK, đã có) | chọn dòng nào để load, không sửa qua PATCH |
| Elevator endpoint | (chưa dùng) | `elevator_endpoint` (đã có) | cho ElevatorBackend (Đề xuất 2) |
| Dải tầng min/max | (chưa dùng) | `floors_min/max` (đã có) | validate floor call |
| Match threshold | `--match-thr` | `match_thr` (thêm) | 0.20–0.55, mặc định 0.35 |
| Confirm streak | `--confirm-streak` | `confirm_streak` (thêm) | ≥1, mặc định 5 |
| Cooldown ms | `--cooldown-ms` | `cooldown_ms` (thêm) | ≥0, mặc định 3000 |
| Unknown timeout ms | `--unknown-after-ms` | `unknown_after_ms` (thêm) | ≥0, mặc định 2000 |
| Reconnect backoff min | `--reconnect-min-ms` | `reconnect_min_ms` (thêm) | mặc định 500 |
| Reconnect backoff max | `--reconnect-max-ms` | `reconnect_max_ms` (thêm) | mặc định 10000 |

### Nhóm B — Admin-only (đổi được nhưng ảnh hưởng an toàn, KHÔNG cho cư dân thường)

Chưa có code (phụ thuộc Giai đoạn 2/3). Ghi tên để version 2 chừa chỗ, triển khai sau:

- Ngưỡng liveness / bật-tắt liveness (Giai đoạn 2).
- TTS on/off, ngôn ngữ mặc định cabin cho khách/unknown (Giai đoạn 2 — Piper).
- Debounce theo tầng cho ElevatorBackend (Đề xuất 2).

### Nhóm C — KHÔNG cho web chỉnh (thuộc thiết bị/triển khai)

Giữ nguyên qua CLI/env, gắn với build + filesystem của Pi. Cho web đụng = mở lỗ hổng hoặc app chết:

- Đường dẫn model (`detect.nb`, `--recog-model`, `--person-model`), `--recog-dim`, `--recog-bgr`
  → sai là app không load được hoặc mọi người thành unknown (Risk R11).
- `--resident-db` / `--face-db` path, `--log-dir`, `--log-level`.
- **`--gst-pipeline` (custom GStreamer pipeline)** → cho web nhập chuỗi pipeline tùy ý là
  command-injection-adjacent. Web chỉ được nhập **URL RTSP**, app tự dựng pipeline bằng
  `build_gst_pipeline()`.
- YOLO/tracker params (`--person-*`, `--track-*`, `--recog-retry`), UI/fullscreen.

## 4. Yêu cầu chức năng

### R1 — Migration schema_version 2
Là hệ thống, tôi cần thêm cột config vào `cabins` mà không mất dữ liệu residents đã enroll.

1. HỆ THỐNG PHẢI cung cấp migration nâng schema từ version 1 → 2, idempotent (chạy lại không lỗi).
2. Migration PHẢI thêm các cột Nhóm A còn thiếu vào `cabins` (`gst_latency_ms`, `match_thr`,
   `confirm_streak`, `cooldown_ms`, `unknown_after_ms`, `reconnect_min_ms`, `reconnect_max_ms`),
   mỗi cột có `DEFAULT` bằng đúng default CLI hiện tại.
3. Migration PHẢI thêm `residents.ext_id` (nullable, unique) — gộp cùng version 2 theo
   `DEVELOPMENT_PLAN.md` §3.5 để không phải migrate 2 lần.
4. Migration PHẢI ghi `INSERT OR IGNORE INTO schema_version (version) VALUES (2)`.
5. KHI mở một DB đang ở version 1, `ResidentDB::open()` PHẢI tự áp migration lên version 2.
6. Cột thêm PHẢI dùng `ALTER TABLE ADD COLUMN` (SQLite giữ dữ liệu cũ); KHÔNG drop/recreate `cabins`.

### R2 — App đọc config vận hành từ DB (`CabinConfig`)
Là cabin, tôi cần app chạy theo config lưu trong DB, không phải hardcode CLI.

1. KHI khởi động ở chế độ `--resident-db`, HỆ THỐNG PHẢI đọc dòng `cabins` có `id = cabin_id`
   và nạp các trường Nhóm A vào một struct config runtime.
2. NẾU không tìm thấy dòng cabin tương ứng, HỆ THỐNG PHẢI log cảnh báo và dùng giá trị default
   (không được crash).
3. HỆ THỐNG PHẢI parse `camera_urls` (JSON array); v1 dùng phần tử đầu tiên làm RTSP source.
   NẾU mảng rỗng hoặc parse lỗi, HỆ THỐNG PHẢI fallback về USB cam (`cam_id`) và log cảnh báo.
4. Các giá trị đọc từ DB PHẢI được validate theo ràng buộc R4 trước khi áp dụng; giá trị ngoài
   khoảng PHẢI bị kẹp (clamp) về khoảng hợp lệ + log, KHÔNG để giá trị bẩn làm cabin hành xử sai.

### R3 — Precedence CLI > DB > default
Là dev, tôi cần override config bằng CLI khi test mà không phải sửa DB.

1. Với mỗi tham số Nhóm A, thứ tự ưu tiên PHẢI là: **giá trị CLI (nếu người dùng truyền) > giá trị DB
   (nếu dòng cabin có) > default compile-in**.
2. HỆ THỐNG PHẢI phân biệt được "người dùng KHÔNG truyền CLI" với "người dùng truyền đúng bằng
   default" — chỉ override DB khi CLI thực sự được truyền (tránh CLI default vô tình đè DB).
3. KHI một tham số bị override bởi CLI, HỆ THỐNG PHẢI log rõ nguồn (cli/db/default) để vận hành
   truy vết được vì sao app chạy với giá trị đó.
4. Chế độ `--face-db` (test/dev) KHÔNG đọc config từ `cabins` (không có khái niệm cabin operational);
   chỉ dùng CLI > default.

### R4 — Ràng buộc validate (áp cho cả app đọc DB lẫn REST API ghi DB)
Là hệ thống gọi thang máy, tôi cần config bẩn không làm cabin hành xử nguy hiểm.

1. `match_thr` PHẢI trong [0.05, 0.95]; ngoài khoảng → clamp + log.
2. `confirm_streak` PHẢI ≥ 1.
3. `cooldown_ms`, `unknown_after_ms`, `gst_latency_ms` PHẢI ≥ 0.
4. `reconnect_min_ms` PHẢI ≥ 1 và ≤ `reconnect_max_ms`; nếu min > max → hoán đổi hoặc kẹp + log.
5. `floors_min` PHẢI ≤ `floors_max`.
6. `camera_urls` khi ghi qua API PHẢI là JSON array hợp lệ; mỗi phần tử PHẢI khớp scheme cho phép
   (`rtsp://`, `http://`, `https://`, hoặc chỉ số USB `cam://N`). CHUỖI PIPELINE GStreamer tùy ý
   PHẢI bị từ chối (chống command-injection — Nhóm C).
7. REST API PHẢI từ chối (400) request đổi bất kỳ trường Nhóm C nào (model path, log, gst-pipeline).

### R5 — Đổi RTSP / config khi đang chạy (reload strategy)
Là người vận hành, tôi cần đổi camera RTSP từ web và cabin áp dụng được.

1. Khi web PATCH config qua REST API, HỆ THỐNG PHẢI ghi giá trị mới vào bảng `cabins` (persistent).
2. v1 áp dụng chiến lược **restart-to-apply**: config mới có hiệu lực SAU khi service khởi động lại.
   REST API PHẢI trả cho web biết thay đổi cần restart để có hiệu lực (cờ `restart_required: true`
   trong response).
3. Restart PHẢI do systemd đảm nhiệm (`Restart=always`); app không tự spawn lại chính nó.
4. Việc đổi RTSP KHÔNG được làm mất dữ liệu residents hay match_events (chỉ ghi cột `cabins`).
5. Gián đoạn khi restart (~vài giây) là chấp nhận được ở v1; tài liệu PHẢI ghi rõ hành vi này để đội
   web thông báo cho người dùng.
6. (Tương lai, ngoài scope) hot-reload: `capture_worker` release + `open_capture()` với URL mới —
   spec này để ngỏ đường nhưng KHÔNG triển khai v1.

## 5. Yêu cầu phi chức năng

- **Không hồi quy**: app chạy với DB version 1 cũ (chưa migrate) PHẢI vẫn khởi động được — migration
  tự chạy khi mở. Các CLI args hiện có PHẢI giữ nguyên ý nghĩa.
- **An toàn**: app bind config đọc từ DB nhưng vẫn phải chịu validate R4 — không tin DB tuyệt đối
  (DB có thể bị sửa tay). RTSP URL từ DB đi qua đúng `build_gst_pipeline()`, không nối chuỗi thô.
- **Build**: không thêm dependency ngoài. JSON `camera_urls` parse bằng cách tối giản (mảng chuỗi
  đơn giản) hoặc thư viện header-only đã có; KHÔNG kéo thêm lib nặng cho arm64.
- **Quan sát được**: lúc khởi động app PHẢI log toàn bộ config hiệu lực kèm nguồn (cli/db/default) —
  một dòng tóm tắt để field test đối chiếu.

## 6. Giả định & quyết định

- SQLite là nguồn chân lý cho **cả** dữ liệu residents (spec `resident-db-layer`) **và** config vận
  hành cabin (spec này). Một DB, hai nhóm bảng.
- Config vận hành theo **từng cabin** (`cabins.id`), không phải toàn cục — 2 cabin có thể khác
  threshold/camera.
- v1 mỗi cabin dùng **1 RTSP URL** (phần tử [0] của `camera_urls`); schema giữ dạng array để sau này
  hỗ trợ 2 camera/cabin (`DEVELOPMENT_PLAN.md` nhắc "2 camera trong cabin") mà không phải đổi schema.
- Enroll API và Config API dùng **chung** REST layer (cpp-httplib, bind `127.0.0.1`, sau nginx) — spec
  này không dựng server riêng.
