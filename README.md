# Smart Elevator Cabin AI Core — Orange Pi A733 NPU

Ứng dụng nhận diện khuôn mặt, đếm người, cảnh báo động vật, tự động gọi tầng và ghi nhật ký di chuyển thang máy chạy thời gian thực trên **Orange Pi A733 (NPU VIPLite)**.

**Pipeline vận hành:**
RTSP / USB Camera → Letterbox `640×640` → **YOLOv5s** (person + cat/dog, điều tiết theo cửa thang `8472`) + **ByteTrack** → **SCRFD 2.5g** (`640×640`) → Align 5-landmarks (`112×112`) → **MobileFaceNet** (`512-D`) → **MatchEngine** (multi-embedding max-cosine) → **InteractionManager** → **FloorCallCoordinator** (Socket.IO `8193-<slave>`) + **Occupancy / Traffic Log** (`SQLite WAL`).

---

## 1. Kiến trúc dữ liệu & Quy ước tầng (`schema_version = 4`)

### Nguồn dữ liệu cư dân (`db/residents.db`, schema `db/schema.sql`)
- **Chế độ vận hành (`--resident-db`)**: dùng SQLite WAL làm nguồn dữ liệu chuẩn. Mỗi cư dân có thể có nhiều vector embedding (`source = id_photo | cabin | admin`); `MatchEngine` lấy **max cosine** trên toàn bộ embedding của người đó (không lấy trung bình).
- **Quy ước chỉ số tầng thống nhất** trên `residents` (`home_floor`, `secondary_floor`), `cabins` và thanh ghi gọi tầng `8193-<slave>`:
  - `-1` (`HOME_FLOOR_UNSET` / `SECONDARY_FLOOR_UNSET`): chưa đăng ký tầng (chỉ nhận diện/audit, không tự gọi hướng đó; overlay hiện `F?`).
  - `0`: tầng hầm **`B`** (mã hóa bit `0` → `value = 1`; overlay hiện `FB`). `cabins.floors_min` mặc định là `0`.
  - `1..62`: tầng nổi (mã hóa `value = 2^floor`; ví dụ tầng `5` → `value = 32`).
- **Role tổ chức** (`schema_version 3`): 14 giá trị `snake_case` (`chairman`, `vice_president`, `deputy_general_director`, `head`, `director`, `manager`, `specialist`, `team_leader`, `sales`, `engineer`, `staff`, `intern`, `vip`, `guest_regular`).
- **Cầu nối Web (`deploy/face_core/`)**: dịch vụ Node `face_core` (triển khai tại `/edge/face_core/` dưới PM2, giao diện web `Embody Edge` qua Nginx HTTPS) lưu hồ sơ vào MongoDB và gọi `enroll_bridge.js` → spawn `./add_person --replace --ext-id ... --home-floor ... --secondary-floor ...` ghi atomic vào `db/residents.db`.
- **Chế độ `.fdb` cũ (`--face-db`)**: chỉ giữ cho mục đích test/dev đơn giản (1 vector/người, không tầng, không audit log).

### Hot-reload danh sách cư dân
Ở chế độ `--resident-db`, `ResidentCatalogReloader` kiểm tra thay đổi trên SQLite mỗi `1000 ms` (chỉnh qua `--resident-reload-ms`, đặt `0` để tắt). Khi có thay đổi thực chất về metadata (`name`, `home_floor`, `secondary_floor`, `role`, `ext_id`...) hoặc embedding, app dựng `MatchEngine` mới và hoán đổi tại ranh giới frame mà **không cần restart service**. Các ghi nhận audit (`match_events`, `last_seen_at`, `match_count`) không kích hoạt build lại matcher.

---

## 2. Cấu trúc thư mục

```text
face_recog_app/
├── Makefile                        # Build 6 binary + format/lint targets
├── env.sh                          # Biến môi trường & hàm shell vận hành/dev
├── face-recog-app.service          # Systemd unit chạy production (KillSignal=SIGTERM)
├── AGENTS.md                       # Quy tắc làm việc cho AI agent / developer
│
├── src/
│   ├── main.cpp                    # Vòng lặp frame chính & điều phối các worker
│   ├── app_config.{h,cpp}          # Parse CLI & cấu hình ứng dụng
│   ├── cabin_config.{h,cpp}        # Đọc/hợp nhất config bảng cabins (CLI > DB > default)
│   ├── video_io.{h,cpp}            # Luồng đọc camera USB / RTSP GStreamer + reconnect backoff
│   ├── detect_pre.{h,cpp}          # Letterbox 640×640 BGR uint8
│   ├── yolo_post.{h,cpp}           # Decode YOLOv5s COCO (person + cat/dog)
│   ├── byte_tracker.{h,cpp}        # ByteTrack + Kalman filter + Hungarian assignment
│   ├── person_tracker.{h,cpp}      # Quản lý track người & kết hợp nhận diện theo track
│   ├── face_track_assoc.{h,cpp}    # Ghép bbox mặt SCRFD vào person track
│   ├── scrfd_post.{h,cpp}          # Decode SCRFD 2.5g 9-output + NMS
│   ├── face_align.{h,cpp}          # Căn chỉnh 5-landmarks → 112×112
│   ├── face_recog.{h,cpp}          # NPU inference MobileFaceNet 512-D L2-norm
│   ├── resident_db.{h,cpp}         # SQLite WAL, schema migration v1→v4, async event writer
│   ├── resident_catalog.{h,cpp}    # Snapshot validate + FNV-1a digest + hot-reload
│   ├── resident_enroll.{h,cpp}     # Transaction enroll all-or-nothing
│   ├── match_engine.{h,cpp}        # Multi-embedding cosine matcher
│   ├── interaction.{h,cpp}         # State machine streak + cooldown + unknown timeout
│   ├── door_inference.{h,cpp}      # Điều tiết NPU theo trạng thái cửa thang (8472)
│   ├── floor_call_coordinator.{h,cpp} # Chọn tầng chính/phụ theo hướng & chống gọi lặp
│   ├── socketio_client.{h,cpp}     # Socket.IO v4 WebSocket client (CONN_DATA + Client/command)
│   ├── elevator_registers.{h,cpp}  # Giải mã thanh ghi INVT (8465, 8471, 8472, 8480, 8211..8225)
│   ├── elevator_state.{h,cpp}      # State machine điểm dừng & chuyến chạy thang máy
│   ├── occupancy_*.{h,cpp}         # Đếm người ra/vào vùng cửa & lưu trạng thái bền vững
│   ├── call_tracker.{h,cpp}        # Theo dõi vòng đời Car Call / Hall Call
│   ├── trip_assembler.{h,cpp}      # Ghép chuyến đi hành khách + kết quả gọi tầng
│   ├── visitor_gallery.{h,cpp}     # Re-ID khách vãng lai ẩn danh (tùy chọn)
│   ├── traffic_db.{h,cpp}          # SQLite DB nhật ký giao thông thang máy (db/traffic.db)
│   ├── traffic_log_writer.{h,cpp}  # Worker ghi traffic + JPEG mặt + dọn dẹp theo hạn lưu
│   ├── event_video_recorder.{h,cpp}# Worker ghi clip AVI thô khi có người
│   ├── animal_snapshot_writer.{h,cpp} # Worker lưu ảnh cảnh báo chó/mèo
│   ├── system_metrics.{h,cpp}      # Giám sát CPU/RAM/nhiệt độ/disk & health log định kỳ
│   ├── overlay.{h,cpp}             # Vẽ bbox, track ID, nhãn tầng F<home>/<sec> & HUD
│   ├── add_person.cpp              # CLI thêm/cập nhật 1 cư dân vào SQLite (dùng cho web/CLI)
│   ├── enroll_faces.cpp            # CLI nạp hàng loạt thư mục ảnh vào SQLite
│   ├── capture_person.cpp          # CLI chụp mẫu khuôn mặt từ camera
│   ├── traffic_admin.cpp           # CLI xuất CSV & quản trị visitor trên db/traffic.db
│   ├── probe_models.cpp            # CLI kiểm tra input/output tensor của file .nb
│   └── log/
│       ├── logger.{h,cpp}          # Logger ứng dụng (xoay file theo ngày, giữ 14 ngày)
│       └── log.h                   # Shim tắt log ồn của NPU SDK (KHÔNG chỉnh sửa)
│
├── deploy/face_core/               # Cầu nối Node (face_core.js, enroll_bridge.js) đồng bộ /edge/face_core
├── scripts/get_camera_url.js       # Lấy RTSP URL từ bảng elevator_cameras (PostgreSQL)
├── config/                         # Cấu hình vùng đếm người (occupancy_cam<id>.json)
├── db/                             # schema.sql, traffic_schema.sql (các file *.db gitignored)
├── model/                          # scrfd_2.5g, w600k_mbf, yolov5s_rt (.nb cho A733)
├── docs/Address_Elevator.md        # Bảng tham chiếu thanh ghi biến tần thang máy INVT
└── tests/                          # Unit tests độc lập + run_tests.sh
```

---

## 3. Build, Kiểm tra Style & Unit Test

Tất cả lệnh chạy từ thư mục gốc của repo:

```bash
# Build cả 6 binary: face_recog_app, enroll_faces, capture_person, add_person, probe_models, traffic_admin
make -j4

# Chạy toàn bộ unit test (output biên dịch ra /tmp/scabin_tests)
bash tests/run_tests.sh

# Kiểm tra định dạng code & static analysis (cần cài clang-format, clang-tidy)
make format-check
make lint
```

**Yêu cầu môi trường build:**
- Orange Pi A733 với NPU driver VIPLite (`libVIPhal`, `libNBGlinker`) và `/home/orangepi/ai-sdk`.
- OpenCV 4 (`pkg-config opencv4`), SQLite3 (`libsqlite3-dev`), GStreamer RTSP plugins.

---

## 4. Vận hành & Sử dụng nhanh với `env.sh`

### Dịch vụ systemd (Production)
Service `face-recog-app.service` khởi chạy qua `source env.sh && face_cabin` (lưu ý ứng dụng chủ động bỏ qua `SIGINT`, chỉ dừng bằng `SIGTERM`):

```bash
sudo systemctl status face-recog-app --no-pager
sudo systemctl restart face-recog-app
journalctl -u face-recog-app -f
```

### Các hàm tiện ích sau khi `source env.sh`

```bash
source env.sh

# --- Vận hành cabin ---
face_cabin [URL] [DB] [THR]          # Chạy đầy đủ qua RTSP (tự lấy URL từ PostgreSQL nếu bỏ trống)
face_residents [DB]                  # Xem danh sách cư dân trong SQLite
face_events [DB] [N]                 # Xem N bản ghi match_events gần nhất (mặc định 20)
face_set_resident NAME [FLAGS]       # Sửa nhanh metadata cư dân (--floor, --greeting, --role, --active...)

# --- Quản lý dữ liệu cư dân (SQLite db/residents.db) ---
face_capture NAME [COUNT] [MIN_PX]   # Chụp ảnh mẫu từ USB cam vào faces/<NAME>/
face_add NAME IMG [IMG...] [--replace|--merge]  # Thêm/cập nhật cư dân từ file hoặc thư mục ảnh
face_enroll [DIR] [DB]               # Nạp toàn bộ faces/<subfolder>/ vào SQLite (mỗi ảnh 1 vector)

# --- Dev / Benchmark ---
face_usb [DB] [THR]                  # Chạy pipeline đầy đủ (YOLO + tracker + recog) trên USB cam
face_run_lite [DB] [THR]             # Chạy SCRFD-only (không YOLO/tracker) trên USB cam
face_run_rtsp URL [DB] [THR] [LAT]   # Chạy pipeline đầy đủ với RTSP URL truyền tay
face_detect                          # Chạy riêng SCRFD detect-only
face_bench [N]                       # Benchmark N frames pipeline đầy đủ (USB)
face_bench_lite [N]                  # Benchmark N frames SCRFD-only (USB)
face_bench_rtsp URL [N] [DB]         # Benchmark N frames trên luồng RTSP
face_ls                              # Liệt kê file DB và số ảnh trong faces/
face_help                            # In hướng dẫn tóm tắt
```

### Dùng trực tiếp CLI `./add_person` (hỗ trợ tầng chính, tầng phụ & tầng hầm `B`)
```bash
./add_person --name "Nguyen Van A" --ext-id "NV001" \
    --image /home/orangepi/faces/floor_6/NV001 \
    --home-floor 6 --secondary-floor B --role engineer \
    --db db/residents.db --schema db/schema.sql \
    --det-model model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb \
    --recog-model model/face_recog/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 --recog-bgr --replace
```
*(Cờ `--home-floor` và `--secondary-floor` nhận `B` = `0`, số nguyên `0..62`, hoặc `-1` = chưa đăng ký.)*

---

## 5. Cấu hình vận hành (`cabins`) & Thứ tự ưu tiên

Cấu hình runtime của từng cabin lưu trong bảng `cabins` (đọc theo `--cabin-id` lúc khởi động), thứ tự ưu tiên: **CLI > SQLite `cabins` > Compile default** (thay đổi áp dụng khi restart):
- Các cột trong `cabins`: `camera_urls`, `gst_latency_ms` (100), `match_thr` (0.35), `confirm_streak` (5), `cooldown_ms` (3000), `unknown_after_ms` (2000), `reconnect_min_ms` (500), `reconnect_max_ms` (10000), `floors_min` (0), `floors_max` (30), `elevator_endpoint`.
- **Lưu ý khi chạy qua `face_cabin` (`env.sh`)**: nếu không truyền URL trên dòng lệnh và không đặt `FACE_CABIN_RTSP_URL`, `env.sh` gọi `scripts/get_camera_url.js` lấy URL từ bảng `elevator_cameras` (PostgreSQL, chọn theo `FACE_CAMERA_SELECT`, mặc định `#6`) rồi truyền vào `--source` — do đó ghi đè `cabins.camera_urls`.

---

## 6. Tự động gọi tầng & Điều tiết suy luận theo cửa thang

Khi bật Socket.IO (`FACE_CABIN_SOCKETIO=1` và `FACE_IOT_WS='ws://127.0.0.1:3000/socket.io/?EIO=4&transport=websocket'`), ứng dụng kết nối tới IoT Core để vừa đọc trạng thái thanh ghi (`CONN_DATA`), vừa gửi lệnh gọi tầng (`Client/command` lên `FACE_IOT_FLOOR_ADDRESS`, mặc định `8193-2`).

### Điều tiết NPU theo cửa (`door_inference`)
- Đọc thanh ghi `8472-<slave>` (bit 5: mở cửa, bit 6: đóng cửa).
- Khi cửa mở → bật YOLO xác nhận người → chạy SCRFD + MobileFaceNet theo từng track. Khi cửa đóng và hết người sau thời gian trễ, tạm nghỉ suy luận (giữ nguyên NPU context trong RAM). Nếu mất tín hiệu cửa thì chuyển sang quét dự phòng thưa. Có thể tắt bằng `FACE_ADAPTIVE_INFERENCE=0` hoặc `--no-adaptive-inference`.

### Logic chọn tầng đích chính / phụ (`FloorCallCoordinator`)
`CabinFloorTracker` theo dõi tầng hiện tại `cur` (`8465`) và hướng di chuyển `Up / Down / Unknown` (ưu tiên nút gọi sảnh `8215`/`8225` với bộ nhớ 15 giây sau khi đèn tắt, dự phòng sang bit hướng của `8480`):
- **Chưa có tầng nào (`home < 0` và `secondary < 0`)**: không gọi (`no_floor`).
- **Chưa rõ tầng hiện tại của cabin** (mới khởi động / đứt kết nối): fallback gọi `home_floor` (nếu có).
- **Cư dân có đủ 2 tầng** (đặt `low = min(home, sec)`, `high = max(home, sec)`, `primary = home_floor`):
  - `cur < low`: hướng `Up`/`Unknown` hoặc đang ở tầng đáy (`floors_min`) → gọi `primary`; hướng `Down` (ở trên tầng đáy) → không gọi.
  - `cur == low`: hướng `Up` → gọi `high`; hướng `Down` → gọi `high` nếu đang ở tầng đáy, ngược lại không gọi; hướng `Unknown` → không gọi nếu `cur == primary`, ngược lại gọi `high`.
  - `low < cur < high` (tầng trung gian): hướng `Up` → gọi `high`; hướng `Down` → gọi `low`; hướng `Unknown` → gọi `primary`.
  - `cur == high`: hướng `Down` → gọi `low`; hướng `Up` → không gọi; hướng `Unknown` → không gọi nếu `cur == primary`, ngược lại gọi `low`.
  - `cur > high`: hướng `Down` hoặc `Unknown` → gọi `primary`; hướng `Up` → không gọi.
- **Cư dân chỉ có 1 tầng (`only_floor`)**: ở dưới gọi lên khi `Up`/`Unknown` (hoặc ở tầng đáy); ở trên gọi xuống khi `Down`/`Unknown`; đang ở đúng tầng hoặc đi ngược hướng thì không gọi.
- **Chống gọi lặp theo chuyến (`visit`)**: trong một phiên có người (đóng sau `--floor-session-empty-ms`, mặc định `5000 ms` cabin trống), mỗi `resident_id` chỉ xử lý 1 lần và mỗi tầng đích chỉ gửi 1 lệnh (`requestId = ai-core:<cabin>:<visit>:<seq>`).

---

## 7. Các tính năng giám sát & nhật ký bổ trợ

### Ghi video sự kiện có người (`EventVideoRecorder`)
- Mặc định **tắt** trong `env.sh` (`FACE_RECORD_DIR=""`). Đặt `export FACE_RECORD_DIR=recordings` trước khi `source env.sh` (hoặc truyền `--record-dir DIR`) để ghi clip AVI thô (trước khi vẽ overlay) kèm `1000 ms` pre-roll và đóng sau `3000 ms` cabin trống.

### Cảnh báo động vật (chó / mèo)
- Tận dụng đầu ra 80 lớp của YOLOv5s COCO để phát hiện mèo (lớp 15) và chó (lớp 16) với ngưỡng độc lập `--animal-thr` (`FACE_ANIMAL_THR`, mặc định `0.8`), không tốn thêm lượt chạy NPU.
- Ghi cảnh báo `[animal]` (cooldown 3 giây/loài) và lưu ảnh snapshot vào `recordings/animals/YYYY-MM-DD/` qua worker nền (`--animal-dir ""` để tắt).

### Đếm người (`occupancy`) & Nhật ký di chuyển (`elevator-traffic-log`)
- Khi có file cấu hình vùng cửa `config/occupancy_cam<cabin>.json` (`--occupancy-config`), bộ đếm theo dõi số người ra/vào cabin và lưu bền vững tại `db/occupancy_state_cam<cabin>.json`.
- Khi bật thêm `FACE_TRAFFIC_DB="$FACE_DB_DIR/traffic.db"` (`--traffic-db`), hệ thống ghi lại điểm dừng (`cabin_stops`), chuyến chạy (`cabin_runs`), lượt đi hành khách (`passenger_trips`), và cuộc gọi tầng (`car_calls`, `hall_calls`) vào `db/traffic.db` (`db/traffic_schema.sql`).
- Công cụ quản trị & xuất báo cáo:
  ```bash
  ./traffic_admin --help
  ./traffic_admin --db db/traffic.db export --out-dir /mnt/usb/traffic_export --from 2026-11-01 --to 2026-11-30
  ./traffic_admin --db db/traffic.db visitors list --status active --limit 20
  ./traffic_admin --db db/traffic.db visitors suggest --resident-db db/residents.db
  ./traffic_admin --db db/traffic.db visitors promote 42 --resident-id 7 --resident-db db/residents.db
  ```
- **Lưu ý pháp lý (Luật Bảo vệ dữ liệu cá nhân số 91/2025/QH15)**: ảnh mặt và embedding của người chưa đăng ký là dữ liệu sinh trắc học. Trước khi bật `FACE_TRAFFIC_VISITOR_REID=1` hoặc `FACE_TRAFFIC_SAVE_FACES=visitors|all` trên hệ thống vận hành, đơn vị quản lý cần hoàn tất thông báo tại thang máy, xác lập căn cứ pháp lý, kiểm soát quyền truy cập file (`0600`) và tuân thủ thời hạn lưu giữ (`--visitor-retention-days 90`, `--face-retention-days 30`).

---

## 8. Hệ thống Log vận hành

Ứng dụng ghi log qua `src/log/logger.h` ra `stderr` và file xoay vòng theo ngày (mặc định lưu 14 ngày tại `$FACE_CABIN_LOG_DIR`, ví dụ `logs/face-cabin-YYYY-MM-DD.log`). **Tuyệt đối không ghi PII** (không tên cư dân, căn hộ, đường dẫn ảnh cá nhân hay vector).

Các nhóm tag chính:
- `[sys]` (chu kỳ 30s): tài nguyên hệ thống & tiến trình (`uptime_s`, `cpu`, `proc_cpu`, `ram`, `rss_mb`, `disk`, nhiệt độ, `load`, `threads`, `fd`).
- `[traffic] health` (chu kỳ 30s khi bật traffic): `occupancy`, `open_trips`, `open_calls`, `visitors`, `reg_age_ms`, `reg_inv`, `run_mismatch`, `q`, `drop`, `err`.
- `[occupancy] health` (chu kỳ 30s khi chỉ bật occupancy, tắt traffic): `count`.
- `[metrics]` & `[scheduler]`: FPS và độ trễ từng chặng (`cap`, `pre`, `yolo`, `scrfd`, `track`, `recog`, `e2e`) cùng trạng thái đóng/mở suy luận theo cửa.
- `[event]`, `[floor-call]`, `[db-reload]`, `[animal]`, `[socketio]`, `[cam]`: các sự kiện vận hành theo thời gian thực.

---

## 9. Thông tin Model NPU & Tinh chỉnh ngưỡng

| Thành phần | File `.nb` mặc định | Input | Vai trò |
| :--- | :--- | :--- | :--- |
| **YOLOv5s COCO** | `model/person_det/yolov5s_rt_uint8_a733.nb` | `640×640` | Phát hiện người (cho ByteTrack / occupancy) & chó/mèo |
| **SCRFD 2.5g** | `model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb` | `640×640` | Phát hiện khuôn mặt + 5 điểm mốc (16800 anchors) |
| **MobileFaceNet** | `model/face_recog/w600k_mbf_uint8_a733.nb` | `112×112` (`--recog-bgr`) | Trích xuất vector đặc trưng `512-D` chuẩn hóa L2 |

**Gợi ý ngưỡng `--match-thr`** (mặc định `0.35`):
- `0.30`: nới lỏng (dùng khi ảnh đăng ký ít góc chụp, ánh sáng cabin khó).
- `0.35`: cân bằng mặc định cho vận hành.
- `0.45+`: chặt hơn để giảm tối đa nhận nhầm khi có đông khách vãng lai.
