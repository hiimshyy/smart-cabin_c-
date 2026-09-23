# Smart Elevator Cabin — Kế hoạch phát triển

**Dự án**: Face Recognition Smart Cabin for Elevator
**Timeline**: 25/8/2026 → 24/10/2026 (8 tuần + go-live)
**Hardware**: Orange Pi A733 (NPU)
**Target**: 2 cabins, ~1000 residents, FAR 1/1000
**Team**: 1 developer (gộp vai trò Dev A + Dev B so với plan ban đầu)

---

## 1. Scope Summary

**Chức năng chính**:

1. Nhận diện cư dân qua 2 camera trong cabin
2. Auto-select tầng dựa trên `home_floor` của resident
3. TTS chào bằng VI hoặc EN (theo preference)
4. Fallback manual buttons luôn hoạt động
5. **Enroll API** cho web đăng ký khuôn mặt (đội web gọi vào, người dùng chụp bằng điện thoại)
6. **Gọi tầng thật qua ConnCore** (Modbus RTU / RS485 → Elevator Controller)

**Đã đóng scope (v1)**:

- ❌ STT/voice command (chỉ TTS)
- ❌ Elevator vendor SDK (đi qua ConnCore, không tích hợp SDK hãng)
- ❌ Q&A / conversational AI
- ❌ Weather / news / display extras
- ❌ Display UI / LCD touchscreen (không làm ở v1)
- ❌ ElevCore sync với app AIoT (đẩy sang phase sau — xem §3.5)

**Data source**:

- Enrollment phase 1: **web local** → API `POST /api/v1/enroll` → SQLite trên Orange Pi
- Enrollment phase sau: app AIoT → ElevCore → đẩy xuống local
- Self-supervised: capture in-cabin embedding sau khi confirm để cải thiện dần

---

## 2. Kiến trúc

### 2.1 Sơ đồ tổng thể

```Markdown
Mọi thành phần chạy **trên Orange Pi A733**, trừ điện thoại và Elevator Controller.

```Markdown
   ┌──────────────┐
   │  Điện thoại  │  cư dân / nhân viên — đăng ký khuôn mặt
   │   browser    │
   └──────┬───────┘
          │ HTTPS :443 (LAN công ty)
          ▼
   ┌───────────────────────┐
   │    nginx / Caddy      │  ← TLS terminate + serve web app (đội web)
   │  /      → static web  │
   │  /api/* → :8080       │
   └───────────┬───────────┘
               │ 127.0.0.1:8080 + Bearer token
   ┌────────┐  │
   │ Camera │  │
   └───┬────┘  │
       ▼       ▼
   ┌──────────────────────────────────────────────────────┐
   │               face_recog_app (daemon)                │
   │  NPU: SCRFD detect + MobileFaceNet recog             │
   │  + Tracker + MatchEngine + InteractionManager        │
   │  + Enroll REST API (cpp-httplib) — LOOPBACK ONLY     │
   └──┬───────────┬───────────┬──────────────┬────────────┘
      │           │           │              │ UDS + JSON Lines
      │           │           │              │ /run/elev/conncore.sock
      ▼           ▼           ▼              ▼
  ┌────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
  │ Piper  │ │  SQLite  │ │  System  │ │  ConnCore    │  (đội IoT)
  │  TTS   │ │ residents│ │   Log    │ │  Modbus RTU  │
  │ VI+EN  │ │ + events │ │ /var/log │ │  RS485       │
  └────────┘ └──────────┘ └──────────┘ └──────┬───────┘
                  ▲                           │ RS485
                  │                           ▼
        ┌─────────┴──────────┐    ┌──────────────────┐
        │ ElevCore (đội IoT)│    │     Elevator     │
        │ PHASE SAU:         │    │    Controller    │
        │ app AIoT ↔ cloud  │    └──────────────────┘
        └────────────────────┘
```

### 2.2 Pipeline thực tế đã implement (tính đến 3/9/2026)

```
USB/RTSP Camera
  │ capture thread (latest-frame slot)
  ▼
Letterbox 640×640 (detect_pre)
  │
  ├── [opt] YOLO person detect → Tracker (IoU, ghost list)
  │
  ▼
SCRFD 2.5g face detect (~10-17ms NPU)
  │
  ▼
Align 5-landmark → 112×112 (face_align)
  │
  ▼
MobileFaceNet embedding 512-D (~3ms NPU)
  │
  ├── MatchEngine: cosine-max vs ALL embeddings/resident (multi-embedding, KHÔNG avg)
  │                 → resident_id + similarity
  │
  ├── InteractionManager: state machine DETECTING→MATCHED→CONFIRMED
  │                       + cooldown per resident (chống flip-flop)
  │                       + unknown timeout (audit)
  │
  ├── ResidentDB (SQLite/WAL): ghi match_events async (writer thread batch)
  │                            + touch_resident (last_seen_at, match_count)
  │
  ├── Logger (self-written): file /var/log/face-cabin/face-cabin-YYYY-MM-DD.log
  │                          + stderr (color TTY), level TRACE..ERROR, retention 14d
  │
  ├── ElevatorBackend (⏳ GĐ1-close): mock (log) | conncore (UDS) — swappable
  │                                   → call_floor(home_floor) khi CONFIRMED
  │
  └── (v1 không có Display UI — chỉ TTS + gọi tầng)
```

**Data flow per interaction** (target < 2s):

```
frame → detect (10-17ms) → align (1ms) → recog (3ms) →
  match (≤2ms, 10k vectors) → interaction (0.01ms) → log_event (async)
  → ConnCore call_floor (ack ≤500ms) + TTS
```

**Enroll flow** (NPU dùng chung với frame loop, serialize bằng mutex):

```
điện thoại chụp → web backend → POST /api/v1/enroll (multipart, Bearer token)
  → face_recog_app: decode → SCRFD detect largest → align → embedding
  → INSERT residents + embeddings (source='id_photo')
  → 200 {resident_id, embeddings_added, rejected[]}
```

Mỗi ảnh enroll tốn ~18ms NPU (detect 15 + recog 3). Gửi 3 ảnh ≈ 60ms → mất 1-2 frame của
vòng nhận diện, chấp nhận được. Giới hạn upload: ≤5 ảnh, ≤8MB/request để không OOM trên Pi.

---

## 3. Tech Stack

| Layer          | Library                                                       | Version    | Trạng thái   |
| -------------- | ------------------------------------------------------------- | ---------- | -------------- |
| Detect         | awnn_lib +**SCRFD 2.5g** (thay RetinaFace)              | current    | ✅ Đã có    |
| Recog          | awnn_lib + MobileFaceNet (w600k_mbf 512-D)                    | current    | ✅ Đã có    |
| Person/Track   | YOLO person (opt) + Tracker IoU tự viết                     | current    | ✅ Đã có    |
| Matching       | **MatchEngine** multi-embedding (tự viết)             | —         | ✅ Đã có    |
| Interaction    | **InteractionManager** state machine (tự viết)        | —         | ✅ Đã có    |
| Video capture  | GStreamer + OpenCV                                            | 1.22 / 4.6 | ✅ Đã có    |
| Database       | SQLite3 + WAL (`resident_db`, async writer)                 | 3.x        | ✅ Đã có    |
| Migration      | `migrate_fdb` (.fdb → SQLite, 1 chiều)                    | —         | ✅ Đã có    |
| Logging        | **Self-written logger** (không dùng spdlog)           | —         | ✅ Đã có    |
| Enroll API     | **cpp-httplib** (nhúng trong face_recog_app, loopback) | latest     | ⏳ Ưu tiên 1 |
| Reverse proxy  | nginx hoặc Caddy (TLS + serve web app)                       | —         | ⏳ Ưu tiên 1 |
| Web đăng ký | Đội web — chạy trên Pi sau proxy (stack chưa chốt)     | —         | ⏳ Đội web   |
| Elevator       | **ElevatorBackend**: mock \| ConnCore (UDS+JSONL)       | —         | ⏳ Ưu tiên 1 |
| TTS            | Piper (offline)                                               | latest     | ⏳ Chưa làm  |
| Config         | YAML (hoặc giữ CLI args — xem ghi chú)                    | —         | ⏳ Chưa làm  |
| Metrics        | prometheus-cpp                                                | —         | ⏳ Chưa làm  |
| Service        | systemd                                                       | —         | ⏳ Ưu tiên 2 |
| Cloud sync     | ElevCore (đội IoT)                                          | —         | ⏸ Phase sau   |
| Build          | Makefile                                                      | —         | ✅ Đã có    |

### 3.1 Quyết định: Logger tự viết thay vì spdlog

| Tiêu chí            | spdlog                                      | Logger tự viết (đã chọn)                                              |
| --------------------- | ------------------------------------------- | -------------------------------------------------------------------------- |
| Dependency            | Thêm lib/submodule, cần setup trên arm64 | Không dependency ngoài C++17 std + POSIX                                 |
| Build trên Orange Pi | Phức tạp hơn (cross-compile / apt)       | Compile thẳng cùng Makefile, đã verify                                 |
| Tính năng           | Rất phong phú (async, backtrace, fmt)     | Đủ: level, thread-safe, stderr+file, rotation/ngày, retention, màu TTY |
| Hiệu năng           | Async queue tối ưu cao                    | fflush/record (INFO+ thưa nên OK); bản ghi bị lọc gần miễn phí     |
| Bảo trì             | Community                                   | Tự duy trì (~350 dòng, đơn giản)                                     |

**Kết luận**: dự án nhúng 1 app / 1 dev → logger tự viết ít ma sát build, không dependency, đủ tính năng
vận hành 24/7. spdlog chỉ đáng dùng khi cần multi-sink phức tạp (syslog remote, Loki) hoặc throughput
log cực cao — chưa cần ở v1. Chi tiết design: `.kiro/specs/system-logging/`.

### 3.2 Config vận hành: DB-driven (KHÔNG dùng YAML)

Config vận hành cabin giờ **DB-driven** (bảng `cabins`, schema_version 2) — đã implement (spec
`cabin-runtime-config` Task 1–4: migration + `load_cabin`/`update_cabin_config` + module
`cabin_config` resolve/validate + khâu nối `main.cpp`). Log/model path vẫn qua CLI args + env
(`--resident-db`, `--cabin-id`, `--log-level`, `--log-dir`, `FACE_CABIN_LOG_*`... — Nhóm C).

**Quyết định (16/9): bỏ ý tưởng `config.yaml`, chuyển config vận hành sang DB-driven.** Lý do: web/app
cần đổi được config từng cabin từ xa (quan trọng nhất là **link RTSP**), mà web ghi được vào **SQLite**
(cùng DB residents) chứ không ghi được vào file YAML trên Pi. Bảng `cabins` trong `db/schema.sql` đã có
sẵn `camera_urls`, `elevator_endpoint`, `floors_min/max` cho đúng mục đích này.

- **Nguồn chân lý config vận hành = bảng `cabins`** (per-cabin). App đọc lúc khởi động; precedence
  **CLI > DB > default** (CLI để dev override, DB để web chỉnh, default compile-in).
- Web chỉ chỉnh **config vận hành** (RTSP, match_thr, cooldown, confirm_streak, latency, dải tầng,
  elevator endpoint). **KHÔNG** cho web đụng model path / log / GStreamer pipeline (bảo mật).
- Đổi RTSP khi đang chạy 24/7: **restart-to-apply** (systemd `Restart=always`), không hot-reload ở v1.

Chi tiết spec: `.kiro/specs/cabin-runtime-config/` (migration schema_version 2, `CabinConfig`, resolve
precedence, REST `GET/PATCH /api/v1/cabins/{id}`, luồng đổi RTSP).

### 3.3 Giao tiếp với ConnCore (Modbus RTU / RS485)

ConnCore do **đội IoT** viết (C++), **đọc + ghi** Modbus RTU qua RS485 tới Elevator Controller.
Tích hợp lên Orange Pi **đầu tuần 21/9**. Hai service chạy cùng thiết bị.

**Transport đề xuất**: Unix domain socket `SOCK_STREAM` tại `/run/elev/conncore.sock`,
payload **JSON Lines** (1 JSON object / dòng, phân tách `\n`).

| Tiêu chí     | UDS (đề xuất)                                 | TCP localhost                                |
| -------------- | ------------------------------------------------ | -------------------------------------------- |
| Phơi ra mạng | Không có mặt trên network stack              | Vẫn tiếp cận được qua SSH port-forward |
| Access control | Filesystem: mode`0660`, group `elev`         | Phải tự viết auth                         |
| Port conflict  | Không có                                       | Phải cấp/quản lý port                    |
| Lifecycle      | systemd`RuntimeDirectory=elev` tự dọn        | —                                           |
| Debug          | `socat - UNIX-CONNECT:/run/elev/conncore.sock` | `nc 127.0.0.1 <port>`                      |
| Latency        | ~µs (không đáng kể so với budget 2s)       | ~µs                                         |

Chọn JSON Lines thay vì gRPC/protobuf: không codegen, không thêm dependency arm64, đội IoT
debug/viết tool Python được ngay.

**Hợp đồng tin nhắn** (chờ đội IoT review):

```jsonc
// face_recog_app → ConnCore
{"v":1,"id":"c-1042","type":"call_floor","cabin_id":1,"floor":7,"source":"face","ts":"..."}
{"v":1,"id":"c-1043","type":"get_state","cabin_id":1}
{"v":1,"id":"c-1044","type":"subscribe","topics":["state"]}

// ConnCore → face_recog_app
{"v":1,"id":"c-1042","type":"ack","ok":true}
{"v":1,"id":"c-1042","type":"ack","ok":false,"err":"floor_out_of_range"}
{"v":1,"type":"state","cabin_id":1,"current_floor":3,"door":"closed","direction":"up","moving":true,"ts":"..."}
{"v":1,"type":"heartbeat","ts":"..."}
```

**Yêu cầu bắt buộc với ConnCore:**

1. `id` đối chiếu ack — không có thì không làm được timeout/retry, không biết lệnh nào fail
2. `v` (version) — hai service tiến hóa độc lập, không phải deploy đồng thời
3. Push `state` khi đổi trạng thái (không poll) + `heartbeat` ~1s để phát hiện link chết
4. `err` phân biệt được: `modbus_timeout` / `floor_out_of_range` / `busy` → xử lý khác nhau

**Phía face_recog_app đảm nhiệm:**

- `src/elevator.{h,cpp}`: interface `ElevatorBackend` với 2 impl — `mock` (log) và `conncore` (UDS).
  Cho phép dev + test trước khi ConnCore lên máy → **không bị chặn tiến độ**.
- Ack timeout 500ms → ghi `match_events(action='error')`, KHÔNG retry dồn
- Validate `floor` theo `cabins.floors_min/max` trước khi gửi
- Debounce theo tầng (chồng lên cooldown theo resident đã có)
- Reconnect exponential backoff khi ConnCore restart (tái dùng pattern `capture_worker`)
- Nút bấm vật lý luôn hoạt động độc lập — ConnCore chết không được làm thang ngừng dùng

### 3.4 Enroll API cho web đăng ký khuôn mặt

**Phân chia trách nhiệm**: đội web lo UI + login + truy cập camera điện thoại. face_recog_app chỉ
nhận trường thông tin + ảnh và trích embedding (vì cần NPU). API nhúng trong daemon bằng cpp-httplib,
dùng chung NPU context với frame loop (serialize bằng mutex) — tránh xung đột 2 NPU context.

> ⚠️ **API này CHƯA tồn tại** — hiện enroll chỉ có CLI (`enroll_faces`, `add_person`).
> Đội web đang bị chặn chờ hợp đồng này → **ưu tiên số 1**.

```
POST   /api/v1/enroll                       multipart: name, apartment, home_floor,
                                            language, greeting_name, role, images[]
GET    /api/v1/residents?q=                 tra trùng / liệt kê
PATCH  /api/v1/residents/{id}               sửa home_floor, greeting_name
POST   /api/v1/residents/{id}/embeddings    thêm ảnh cho người đã có
DELETE /api/v1/residents/{id}               soft delete (active=0) — right-to-delete (R7)
GET    /api/v1/health                       NPU ok / DB ok
```

Response enroll trả lý do từng ảnh bị loại để web cho người dùng chụp lại:

```json
{"ok":true,"resident_id":238,"embeddings_added":2,
 "rejected":[{"index":1,"reason":"no_face"},{"index":3,"reason":"face_too_small"}]}
```

Lý do loại: `no_face`, `multiple_faces`, `face_too_small` (<100px), `low_quality`, `decode_failed`.

**Topology (đã chốt 16/9): web chạy TRÊN Orange Pi**, sau reverse proxy:

```
điện thoại ──HTTPS :443──> nginx/Caddy (trên Pi)
                              ├── /        → static web app (đội web)
                              └── /api/*   → 127.0.0.1:8080 (face_recog_app)
```

**Bảo mật:** `face_recog_app` bind **`127.0.0.1` duy nhất**, KHÔNG bao giờ ra LAN. API enroll
không tiếp cận được từ mạng công ty — muốn gọi phải qua nginx, mà nginx đã qua login của web app.
Bearer token vẫn giữ nhưng là lớp phòng thủ thứ hai (defense-in-depth), không còn là rào duy nhất.
→ Giảm mạnh R14.

Auth **người dùng** nằm ở web app, không ở Orange Pi → phase 1 **không cần bảng `users`**,
không cần PBKDF2/session/CSRF trong code C++.

**Không chọn** phương án nhét static file vào cpp-httplib để chỉ có 1 process: sẽ phải tự
implement login/session/CSRF bằng C++ và ép đội web giao static thuần (không dùng được backend/SSR).
Thêm nginx tốn vài MB RAM nhưng tách bạch trách nhiệm — đáng.

**Truy cập camera trên điện thoại** (vấn đề mới do web nằm trên Pi — cert giờ là việc của mình):

| Cách                                                   | HTTPS?      | Nhận xét                                                                                                                                                                                           |
| ------------------------------------------------------- | ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `<input type="file" accept="image/*" capture="user">` | Không cần | Mở app camera gốc → upload file. Không phải camera API nên không cần secure context.**Đề xuất cho pilot 30/9**                                                                      |
| Let's Encrypt qua**DNS-01** + split-horizon DNS   | Có         | DNS-01 xác thực bằng TXT record → không cần Pi công khai internet. Cert hợp lệ cho domain trỏ về IP LAN, điện thoại không phải cài gì.**Giải pháp đúng trước rollout** |
| Self-signed + cài CA vào từng điện thoại          | Có         | ❌ 20 máy test = 20 lần cấu hình tay; người dùng thật không làm được                                                                                                                    |

⚠️ **Rủi ro có chủ ý ở pilot**: chạy HTTP trên LAN nghĩa là ảnh khuôn mặt + session cookie không
mã hoá ở tầng ứng dụng. WPA2 vẫn che được người ngoài mạng, nhưng người đã ở trong mạng có thể
đọc (ARP spoofing). Chấp nhận cho pilot 10-20 người nội bộ; **không chấp nhận cho 1000 cư dân**.

`home_floor` do web gửi; nếu thiếu → default `0` (`HOME_FLOOR_UNSET`: chào tên, không auto-gọi tầng).

**Chống lạm dụng** (quan trọng hơn chọn giao thức — hệ thống này gọi thang máy):

- Tài khoản do HR/admin cấp trước, **không mở self-registration**
- `home_floor` do admin gán, người dùng không tự chọn
- Rate limit trên endpoint enroll: mỗi request tốn thời gian NPU, không để spam làm nghẽn nhận diện
- `Idempotency-Key` từ web: WiFi chập chờn → retry POST → tránh tạo trùng cư dân

**Chất lượng ảnh enroll** (quyết định match rate về sau, siết ngay tại API): đúng 1 mặt,
mặt ≥150px, check blur (Laplacian variance), yêu cầu 3+ ảnh khác góc nhẹ.

> Ghi chú R1: selfie điện thoại gần điều kiện camera cabin hơn ảnh thẻ 3×4 scan → match rate
> pilot có thể tốt hơn kỳ vọng ban đầu. Vẫn giữ self-supervised capture vì selfie gần/sáng đẹp
> vẫn khác cam cabin góc rộng.

### 3.5 ElevCore — phase sau

ElevCore (đội IoT) là cổng giao tiếp cloud/app AIoT.

- **Phase 1 (hiện tại)**: đăng ký ở local qua web. SQLite trên Orange Pi là source of truth.
- **Phase sau**: đăng ký trên app AIoT → ElevCore đẩy xuống local.

**Chuẩn bị trước ngay từ giờ**: thêm cột `ext_id` (ID cư dân phía cloud, nullable, unique) vào
`residents` trong `schema_version = 2`. Rẻ, và tránh phải migrate dữ liệu đã enroll khi bật sync.

---

## 4. Lịch triển khai chi tiết (điều chỉnh cho 1 dev)

> Timeline gốc 6 tuần chia 2 dev. Thực tế 1 dev nên giãn thành ~8 tuần. Foundation + data layer +
> logging đã xong sớm; phần integration (Enroll API, ConnCore) + TTS + ops dồn về sau.

### 🗓 Giai đoạn 1 (25/8 – ~7/9) — Foundation + Data layer + Logging ✅ PHẦN LỚN ĐÃ XONG

**Mục tiêu**: nền tảng dữ liệu SQLite + nhận diện multi-embedding + logging vận hành.

**Đã hoàn thành:**

- [X] Wrap SQLite: `src/resident_db.{h,cpp}` (open WAL + FK, apply schema, load_active, async writer)
- [X] Apply schema `db/schema.sql`, unit test CRUD (ResidentDB 22 checks)
- [X] Migration `.fdb` → SQLite: tool `migrate_fdb` (home_floor sentinel 0, skip/overwrite)
- [X] **MatchEngine** multi-embedding: match max-cosine với TẤT CẢ embedding/người (không avg) — 14 checks
- [X] **InteractionManager** state machine: DETECTING→MATCHED→CONFIRMED, cooldown/resident, unknown timeout — 50 checks
- [X] Khâu nối `main.cpp` chế độ `--resident-db` (match → interaction → match_events)
- [X] Phase 2: `enroll_faces` + `add_person` ghi thẳng SQLite (bỏ `.fdb` khỏi enroll)
- [X] **Logger tự viết** `src/log/logger.{h,cpp}`: level, thread-safe, stderr+file, rotation/ngày, retention — 14 checks
- [X] Di trú toàn bộ binary (main + tool) sang logger
- [X] Code review + fix P1 (logger tầng lib, name-collision→track_id, dim cross-check) + P2-1 (cooldown retry)
- [X] Cài dev libs `libsqlite3-dev` (Orange Pi) / no-sudo extract (máy dev)
**Còn lại của Giai đoạn 1 (chưa làm — GIỮ trong plan):**

- [~] RTSP reconnect logic trong `capture_worker`: exponential backoff — CODE XONG, build OK trên Orange Pi. `open_capture()` dùng chung init+reconnect; sau N read fail liên tiếp (`fail_reopen_threshold=30`) → release + reopen với backoff 500ms→10s (cap, ×2 mỗi lần); backoff ngắt được khi shutdown. Initial-open fail với RTSP KHÔNG thoát app mà vào reconnect loop (quan trọng cho khởi động 24/7 khi camera/mạng chưa sẵn sàng); USB thiếu thiết bị vẫn thoát. CLI `--reconnect-min-ms/--reconnect-max-ms`. **⏳ Test tích hợp PENDING: chờ camera RTSP kết nối lại để xác nhận reconnect <10s + tiếp tục nhận diện sau khi reconnect.**
- [~] Config vận hành DB-driven (thay YAML): migration schema_version 2 + `CabinConfig` đọc từ DB, precedence CLI>DB>default — **XONG phần app** (spec `cabin-runtime-config` Task 1–4, build+129 test pass trên Orange Pi; live RTSP-from-DB gộp đợt test camera pending). Còn REST `GET/PATCH /api/v1/cabins/{id}` (Task 5) — hoãn cùng spec Enroll API (backend `validate_cabin_patch`/`update_cabin_config` đã sẵn).
- [ ] systemd service `face-cabin.service` với `Restart=always` (chưa có)
- [ ] Tool `bulk_enroll`: CSV + `--photos-dir` → SQLite (spec format có ở `docs/BULK_ENROLL_FORMAT.md`, chưa code) — **hạ ưu tiên**, web enroll thay thế cho lần test này
- [ ] SDL2 skeleton `src/cabin_ui.{h,cpp}`: fullscreen 1024×600, font VN UTF-8, 3 màn IDLE/DETECTING/MATCHED (chưa có)
- [ ] Touchscreen tap handler (Waveshare WS170120) (chưa có)

**Deliverables Giai đoạn 1:**

- ✅ SQLite DB enroll được từ ảnh thẻ; multi-embedding matching + audit log hoạt động
- ✅ Logging file rotation/ngày chạy, không PII
- ⏳ SDL2 idle screen trên Waveshare (chưa)
- ⏳ face_recog_app chạy 24h qua systemd (chưa có service)
- ⏳ RTSP mất kết nối tự reconnect <10s (code xong, build OK; test tích hợp pending camera)

---

### 🗓 Giai đoạn 1-close (16/9 – 30/9) — Enroll API + Gọi tầng thật + Field test công ty

**Mục tiêu**: cuối tháng 9 test trực tiếp **nhận diện + tự động gọi tầng** tại thang máy công ty.

**Đánh giá tiến độ (thẳng thắn)**: 16/9 → 30/9 là ~2 tuần cho 1 dev, mà cả 3 hạng mục đều là
integration với hệ thống bên ngoài. Làm hết là không thực tế. Thứ tự dưới đây cắt theo đúng
điều kiện tối thiểu để chứng minh được mục tiêu; ElevCore đẩy sang sau buổi test.

**Ưu tiên 1 — mở chặn cho đội web (làm ngay, tuần 16-20/9):**

- [ ] Chốt + công bố hợp đồng Enroll API (§3.4) cho đội web để họ làm song song
- [ ] `src/http_api.{h,cpp}`: cpp-httplib nhúng trong `face_recog_app`, thread riêng
- [ ] `POST /api/v1/enroll`: multipart → decode → SCRFD largest face → align → embedding → SQLite
- [ ] Mutex quanh NPU context (enroll ↔ frame loop dùng chung 1 core)
- [ ] **Bind `127.0.0.1` only** (không bao giờ `0.0.0.0`) + Bearer token + cap ≤5 ảnh / ≤8MB request
- [ ] Rate limit endpoint enroll + hỗ trợ `Idempotency-Key` (chống tạo trùng khi WiFi chập chờn)
- [ ] Validate ảnh + trả `rejected[]` có lý do (`no_face`, `multiple_faces`, `face_too_small`, `low_quality`)
- [ ] Cấu hình nginx/Caddy trên Pi: TLS :443, `/` → static web, `/api/*` → `127.0.0.1:8080`
- [ ] Đo lại FPS nhận diện khi web + proxy cùng chạy (Pi phải gánh cả realtime 25 FPS)
- [ ] `GET /api/v1/health`, `GET /api/v1/residents`, `PATCH /api/v1/residents/{id}`, `DELETE` (soft)
- [ ] `schema_version = 2`: thêm `residents.ext_id` (chuẩn bị cho ElevCore, xem §3.5)

**Ưu tiên 1 — gọi tầng (song song, không chờ ConnCore):**

- [ ] `src/elevator.{h,cpp}`: interface `ElevatorBackend` + impl `mock` (log)
- [ ] Auto floor-call tại điểm `oc.confirmed` trong `main.cpp`: `home_floor > 0` → gọi tầng;
  `home_floor <= 0` → chỉ chào tên. Ghi `floor_selected` vào `match_events` (hiện đang hardcode 0)
- [ ] Validate floor theo `cabins.floors_min/max` + debounce theo tầng
- [ ] Impl `conncore` backend: UDS client + JSON Lines + ack timeout 500ms + reconnect backoff
- [ ] **Tích hợp thật với ConnCore khi đội IoT lên máy (≈21/9)** — smoke test gọi tầng qua RS485
- [ ] Dùng `state` từ ConnCore để bỏ qua gọi tầng vô nghĩa (đang ở đúng tầng đó / cửa đang mở)

**Ưu tiên 2 — vận hành được trong lúc test:**

- [ ] systemd `face-cabin.service` với `Restart=always` + `RuntimeDirectory=elev`
- [ ] Enroll 10-20 tình nguyện viên công ty qua web + gán `home_floor`
- [ ] Chạy liên tục 24h không crash trước ngày test

**Deliverables Giai đoạn 1:**

- ✅ SQLite DB + multi-embedding matching + audit log hoạt động
- ✅ Logging file rotation/ngày chạy, không PII
- ✅ RTSP mất kết nối tự reconnect <10s (cần verify build trên Orange Pi)
- ⏳ Web đăng ký khuôn mặt từ điện thoại → embedding vào SQLite
- ⏳ Nhận diện → tự động gọi tầng thật qua ConnCore tại thang máy công ty
- ⏳ face_recog_app chạy 24h qua systemd

**Phụ thuộc ngoài (rủi ro tiến độ, không do tôi kiểm soát):**

| Phụ thuộc                          | Bên           | Hạn cần có | Nếu trễ                                           |
| ------------------------------------ | -------------- | ------------- | --------------------------------------------------- |
| ConnCore tích hợp lên Orange Pi   | Đội IoT      | ~21/9         | Test bằng`mock` backend, không gọi tầng thật |
| Web đăng ký khuôn mặt           | Đội web      | ~26/9         | Enroll tạm bằng CLI`add_person`                 |
| RS485 đấu tới Elevator Controller | Đội IoT / KT | ~21/9         | **Chặn hoàn toàn mục tiêu gọi tầng**   |

---

### 🗓 Giai đoạn 2 (~8/9 – 21/9) — Recognition core hoàn chỉnh + Greeting

**Mục tiêu**: E2E: bước vào cabin → chào tên (TTS) → gọi tầng (mock).

- [X] Multi-embedding matching (đã xong ở GĐ1)
- [X] Cooldown per resident (đã xong ở GĐ1 — InteractionManager)
- [X] Event logging `match_events` (đã xong ở GĐ1)
- [ ] Liveness passive: motion diff giữa 2 frame (bbox center > 3px) chống ảnh in tĩnh
  - Optional: NPU liveness model (Silent-Face-Anti-Spoofing) nếu có thời gian

- ↗ Mock elevator + auto floor-call — **đã dồn lên Giai đoạn 1-close** (cần cho test cuối tháng 9)

- [ ] Piper TTS: voice `vi_VN-...` + `en_US-...`, cache audio theo `resident_id`, template VI/EN

**Deliverables:**

- ✅ Person walks in → ~2s: TTS "Chào bác Nga, lên tầng 12" + relay trigger (mock)
- ✅ Ảnh in không match (liveness passive filter)

---

### 🗓 Giai đoạn 3 (~22/9 – 5/10) — Multi-person + i18n + Self-supervised + Ops

**Mục tiêu**: nhiều người, song ngữ, tự cải thiện embedding, vận hành được.

- [ ] Multi-person handler: primary = bbox lớn nhất; 2+ khác tầng → TTS + relay cho tất cả
- [ ] Self-supervised capture: sau confirm (no cancel 3s) → lưu embedding `source='cabin'`, giới hạn 10/người (drop id_photo cũ trước)
- [ ] Threshold auto-adjust: nới (0.30) khi chỉ có id_photo; siết (0.40) khi ≥3 cabin embeddings
- [ ] i18n: template `lang/vi.yaml` / `lang/en.yaml`, switch theo `resident.language`
- [ ] `bulk_enroll` cho đợt enroll 1000 cư dân từ ảnh thẻ HR (web enroll không phù hợp cho số lượng lớn)
- [ ] **ElevCore sync**: app AIoT → cloud → local (dùng `residents.ext_id`, xem §3.5) — chốt giao thức với đội IoT
- [ ] Backup nightly SQLite dump; retention cron `match_events` >30 ngày (VIP 90)

**Deliverables:**

- ✅ 3 người vào cabin → chào + gọi tầng khác nhau
- ✅ VI/EN theo resident
- ✅ Sau ~10 match → in-cabin embeddings được lưu
- ✅ HR bulk enroll từ CSV + ảnh

---

### 🗓 Giai đoạn 4 (~6/10 – 19/10) — REST API + Field Test 1 Cabin

**Mục tiêu**: HR tự quản lý; chạy thật 1 cabin, thu metrics, fix bug.

- [ ] Mở rộng REST API (base đã có ở GĐ1-close): bulk upload, `GET /events`, `GET /metrics` (Prometheus)
- [ ] Backup; retention (auth bearer token đã làm ở GĐ1-close)
- [ ] Error handling: camera offline / DB error / elevator error → fallback manual
- [ ] Emergency mode: 3 miss liên tiếp → auto-fallback manual + log incident
- [ ] Cài trên 1 thang máy (consent BQL), enroll 20-50 volunteers
- [ ] Live monitoring dashboard, consent poster, emergency contact 24/7

**Metrics theo dõi (mục tiêu):**

| Metric                            | Target            |
| --------------------------------- | ----------------- |
| Match rate (registered residents) | >90%              |
| False positive rate               | <0.1%             |
| E2E latency P95                   | <2s               |
| Voice greeting completion         | >95%              |
| System uptime                     | >99%              |
| Cancel/override rate              | <10% (< là tốt) |

---

### 🗓 Giai đoạn 5 (~20/10 – 24/10) — Deploy 2 Cabins + Go-Live

- [ ] Roll out cabin thứ 2, migrate embeddings self-supervised → shared DB
- [ ] Training staff BQL dùng REST API
- [ ] Docs: `docs/RUNBOOK.md`, resident FAQ card, `docs/API.md`
- [ ] Load test 10 concurrent; final security review (no plaintext creds, no PII in logs)

**Deliverables:**

- ✅ 2 cabins live; enroll ~1000 residents; SLA 99.5% uptime, <2s latency, FAR <0.1%

### 🚀 Go-Live ~24/10/2026

- Announcement trước 1 tuần; hotline 24/7 tuần đầu; monitor daily; retrospective sau 1 tháng

---

## 5. Data schema tham chiếu

Xem chi tiết trong `db/schema.sql`. Đặc tả code: `.kiro/specs/resident-db-layer/`.

**Bảng chính:**

- `residents` — 1000 rows target. `home_floor = 0` (sentinel `HOME_FLOOR_UNSET`) = "chưa đăng ký tầng" → chào tên, không auto-gọi tầng
- `embeddings` — 3000-10000 rows (nhiều embedding/người, `source` = id_photo/cabin/admin). MatchEngine lấy max cosine, KHÔNG average
- `cabins` — 2 rows
- `match_events` — ~100-500 rows/ngày, retention 30 ngày. Ghi async qua writer thread (không chặn frame loop)
- `schema_version` — migration tracking

---

## 6. Risk Register

| #   | Risk                                                                           | Prob | Impact             | Mitigation                                                                                                                                                                                           | Trạng thái              |
| --- | ------------------------------------------------------------------------------ | ---- | ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------- |
| R1  | Ảnh thẻ 3×4 không match ảnh cabin                                         | High | High               | Self-supervised update + threshold nới lỏng                                                                                                                                                        | ⏳ (GĐ3)                 |
| R2  | Cư dân già không quen thao tác auto                                       | Med  | Med                | Physical fallback buttons luôn hoạt động; hotline hỗ trợ                                                                                                                                       | ⏳ (GĐ2-3)               |
| R3  | Piper TTS đọc tên VN sai                                                    | Med  | Low-Med            | Custom`greeting_name`, test 100 tên trước                                                                                                                                                       | ⏳ (GĐ2)                 |
| R4  | Multi-person 5+ gây confusion                                                 | Low  | Med                | Chỉ chào top-2 primary, còn lại "và các bạn"                                                                                                                                                  | ⏳ (GĐ3)                 |
| R5  | Liveness bypass bằng video HD                                                 | Med  | High               | Motion diff frame + optional NPU liveness model                                                                                                                                                      | ⏳ (GĐ2)                 |
| R6  | Camera hỏng / mạng chậm                                                     | Med  | Med                | RTSP reconnect + heartbeat + alert                                                                                                                                                                   | ⏳ (GĐ1/3)               |
| R7  | GDPR/PDPA compliance                                                           | Low  | High               | Consent tracking + retention + right-to-delete.**Log không PII đã enforce**                                                                                                                 | 🟡 phần logging done     |
| R8  | Building manager rút consent test                                             | Low  | High               | Backup building 2 nếu building 1 rút                                                                                                                                                               | ⏳                        |
| R9  | NPU crash không recover                                                       | Low  | High               | Watchdog restart service; kernel crash → reboot                                                                                                                                                     | ⏳ (cần systemd)         |
| R10 | ID photo scan chất lượng thấp                                              | High | Med                | Log failures, HR chụp lại subset                                                                                                                                                                   | ⏳ (cần bulk_enroll)     |
| R11 | Enroll sai`--recog-dim` → cả DB unknown                                    | Med  | Med                | **Đã fix**: cross-check dim DB vs model, LOG_ERROR + thoát (P1-3)                                                                                                                           | ✅ done                   |
| R12 | Trùng tên/greeting_name → nhầm người                                     | Med  | Med                | **Đã fix**: dùng `track_id→resident_id`, không suy theo tên (P1-2)                                                                                                                     | ✅ done                   |
| R13 | ConnCore/RS485 trễ hơn 21/9 → không test được gọi tầng                | Med  | High               | Dev trên`mock` backend, swap qua interface `ElevatorBackend`; escalate sớm với đội IoT                                                                                                      | ⏳ (GĐ1-close)           |
| R14 | Enroll API bị lạm dụng → người lạ tự enroll vào hệ thống gọi thang | Low  | **Critical** | **Giảm mạnh nhờ web-on-Pi**: API bind `127.0.0.1` only, chỉ vào được qua nginx (đã qua login). Bearer token là lớp 2. Không mở self-registration, `home_floor` do admin gán | 🟡 mitigated (GĐ1-close) |
| R18 | HTTP trên LAN ở pilot → ảnh mặt + session không mã hoá tầng app       | Med  | Med                | Rủi ro có chủ ý cho pilot 10-20 người (WPA2 che người ngoài mạng).**Bắt buộc HTTPS (LE DNS-01) trước rollout 1000 cư dân**                                                     | ⏳ cần bạn duyệt       |
| R19 | Web + proxy trên Pi ăn CPU/RAM → tụt FPS nhận diện                       | Med  | Med                | Static SPA + nginx thì gần như miễn phí; nếu đội web mang Node runtime (~50-80MB) thì phải đo lại FPS và cân đối                                                                     | ⏳ chờ stack đội web   |
| R15 | NPU 1 core: enroll làm nghẽn vòng nhận diện                               | Low  | Med                | Mutex + cap ≤5 ảnh/request (~90ms, mất 1-2 frame); đo lại khi test                                                                                                                              | ⏳ (GĐ1-close)           |
| R16 | Hợp đồng API chốt muộn → đội web bị chặn                             | High | High               | **Ưu tiên 1**: công bố §3.4 trước khi code xong, để web làm song song                                                                                                                | ⏳ (GĐ1-close)           |
| R17 | Gọi tầng sai/lặp gây khó chịu hoặc nguy hiểm                           | Med  | High               | Validate`floors_min/max`, debounce theo tầng, dùng `state` ConnCore, nút vật lý luôn hoạt động                                                                                          | ⏳ (GĐ1-close)           |

---

## 7. Câu hỏi mở

**Đã trả lời:**

- ✅ Thang máy vendor: skip (mock) · Số cư dân: 1000 · Use case: smart cabin · FAR: 1/1000
- ✅ Team: 1 dev (điều chỉnh) · Hardware: Orange Pi A733 · Timeline: giãn ~24/10
- ✅ Consent: HR handle · Enroll source: ảnh thẻ 3×4 · Language: VI + EN
- ✅ Display UI: không làm ở v1 · Voice: chỉ TTS · Network: LAN
- ✅ Logging: logger tự viết (không spdlog) · Data source: SQLite là nguồn duy nhất (.fdb chỉ để migrate)
- ✅ **Gọi tầng**: qua ConnCore (C++, đọc+ghi Modbus RTU / RS485), đội IoT tích hợp ≈21/9
- ✅ **Web enroll**: đội web lo UI + login; Pi chỉ nhận field + ảnh qua REST API
- ✅ **Web chạy TRÊN Orange Pi** sau reverse proxy; enroll API bind `127.0.0.1` only (§3.4)
- ✅ **ElevCore**: phase 1 enroll local; phase sau đăng ký ở app AIoT rồi đẩy xuống local

**Còn chưa xác định:**

- ⏳ **Chốt transport với đội IoT**: UDS + JSON Lines như §3.3, hay ConnCore đã có interface khác sẵn? (họp thứ 2, 21/9)
- ⏳ **Stack của web app trên Pi**: static SPA + nginx, hay có backend runtime (Node/Python)? → ảnh hưởng RAM/CPU và FPS
- ⏳ **Duyệt HTTP cho pilot** (kèm `<input capture>`) hay làm HTTPS/LE DNS-01 ngay từ đầu? (xem R18)
- ⏳ Schema bổ sung — bạn đang tham khảo đội IoT (`ext_id` cho ElevCore, có thêm gì nữa?)
- ⏳ Thang máy công ty: hãng nào, tủ có sẵn cổng RS485 hay phải đấu thêm? Register map đã có?
- ⏳ Format ảnh thẻ 3×4 cho đợt enroll 1000 người (JPEG scan? kích thước px?)
- ✅ Config: DB-driven qua bảng `cabins` (không YAML), CLI override — đã chốt, xem §3.2 + spec `cabin-runtime-config`

---

## 8. Progress log

### Đã hoàn thành (commit trên `main`)

| Ngày | Hạng mục                                   | Commit / Ghi chú                                 |
| ----- | -------------------------------------------- | ------------------------------------------------- |
| 24/8  | Kickoff, schema SQLite draft, CSV spec       | `db/schema.sql`, `docs/BULK_ENROLL_FORMAT.md` |
| ~/9   | MatchEngine + ResidentDB + unit tests        | `2b9e6cb`                                       |
| ~/9   | Spec resident-db-layer + system-logging      | `7fa2fd7`, `3f03ba2`                          |
| ~/9   | Logger core + rotation/retention tests       | `3c24201`, `95e1d01`                          |
| ~/9   | Phase 1 resident-db: migrate_fdb + main wire | `680e338`, `62a915e`                          |
| ~/9   | InteractionManager state machine             | `98c2e11`                                       |
| ~/9   | Phase 2: enroll_faces + add_person → SQLite | `c3a7ce0`                                       |
| ~/9   | Link logger vào mọi binary (Makefile)      | `d46384c`                                       |
| ~/9   | Di trú main.cpp + tool sang logger          | `5672530`, `633064e`                          |
| 3/9   | Code review findings                         | `aa53d21`                                       |
| 3/9   | Fix review P1-1..P1-3 + P2-1                 | `ab02162`                                       |

### Đang chờ / kế tiếp

- **GĐ1-close (hạn 30/9)**: Enroll API + ElevatorBackend/ConnCore + auto floor-call + systemd
  → mục tiêu test nhận diện + gọi tầng tại thang máy công ty
- Piper TTS, liveness passive (GĐ2)
- Multi-person, self-supervised, i18n, bulk_enroll, ElevCore sync (GĐ3)
- Mở rộng REST API, field test (GĐ4)

### Tham chiếu

- Specs: `.kiro/specs/resident-db-layer/`, `.kiro/specs/system-logging/`
- Code review đang theo dõi: `docs/CODE_REVIEW_resident-db_logging.md` (P2-2, P2-3, P3-1..P3-6 còn mở)

**Cập nhật cuối**: 16/9/2026 — bỏ Display UI khỏi v1; thêm Giai đoạn 1-close (Enroll API,
ConnCore gọi tầng, ElevCore phase sau) mục tiêu field test 30/9; chốt topology web-on-Pi +
enroll API loopback-only
**Owner**: —
**Version**: 2.3
