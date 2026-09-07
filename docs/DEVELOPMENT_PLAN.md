# Smart Elevator Cabin — Kế hoạch phát triển
# Smart Elevator Cabin — Kế hoạch phát triển

**Dự án**: Face Recognition Smart Cabin for Elevator
**Timeline**: 25/8/2026 → 24/10/2026 (8 tuần + go-live)
**Hardware**: Orange Pi A733 (NPU) + Waveshare 7" 1024×600 HDMI touchscreen
**Target**: 2 cabins, ~1000 residents, FAR 1/1000
**Team**: 1 developer (gộp vai trò Dev A + Dev B so với plan ban đầu)

---

## 1. Scope Summary

**Chức năng chính**:

1. Nhận diện cư dân qua 2 camera trong cabin
2. Hiển thị chào cá nhân hóa trên LCD 7" (tên + hoa/animation)
3. Auto-select tầng dựa trên `home_floor` của resident
4. TTS chào bằng VI hoặc EN (theo preference)
5. Tap-to-cancel/change trên touchscreen (3s countdown)
6. Fallback manual buttons luôn hoạt động

**Đã đóng scope (v1)**:

- ❌ STT/voice command (chỉ TTS)
- ❌ Elevator vendor SDK (mock relay/GPIO)
- ❌ Q&A / conversational AI
- ❌ Weather / news / display extras

**Data source**:

- Enrollment: ảnh thẻ 3×4 do HR cung cấp (1000 người)
- Self-supervised: capture in-cabin embedding sau khi confirm để cải thiện dần

---

## 2. Kiến trúc

### 2.1 Sơ đồ tổng thể

```
     ┌──────────────┐     ┌──────────────┐
     │  Camera 1    │     │  Camera 2    │  RTSP LAN
     └──────┬───────┘     └──────┬───────┘
            └──────┬──────────────┘
                   ▼
            ┌─────────────────┐
            │  Vision daemon  │  NPU: SCRFD detect + MobileFaceNet recog
            │ (face_recog_app)│  + YOLO person detect (opt) + Tracker
            └────────┬────────┘
                     │
      ┌──────────────┼──────────────┬──────────────┐
      ▼              ▼              ▼              ▼
  ┌───────┐  ┌──────────────┐  ┌─────────┐  ┌───────────┐
  │ Piper │  │  Display     │  │ Elev.   │  │  SQLite   │
  │ TTS   │  │  Manager     │  │ Mock    │  │  residents│
  │ VI+EN │  │  SDL2 7"     │  │ GPIO/log│  │  + events │
  └───────┘  └──────────────┘  └─────────┘  └───────────┘
                                              ▲
                                    ┌─────────┘
                                    ▼
                             ┌─────────────┐
                             │ System Log  │  file rotation/day
                             │ /var/log/   │  + stderr/journald
                             └─────────────┘
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
  └── Display thread (OpenCV imshow) — sẽ thay bằng SDL2 ở bước sau
```

**Data flow per interaction** (target < 2s):

```
frame → detect (10-17ms) → align (1ms) → recog (3ms) →
  match (≤2ms, 10k vectors) → interaction (0.01ms) → log_event (async) → TTS+Display+Relay
```

---

## 3. Tech Stack

| Layer         | Library                                    | Version    | Trạng thái |
| ------------- | ------------------------------------------ | ---------- | ---------- |
| Detect        | awnn_lib + **SCRFD 2.5g** (thay RetinaFace) | current    | ✅ Đã có |
| Recog         | awnn_lib + MobileFaceNet (w600k_mbf 512-D) | current    | ✅ Đã có |
| Person/Track  | YOLO person (opt) + Tracker IoU tự viết    | current    | ✅ Đã có |
| Matching      | **MatchEngine** multi-embedding (tự viết)  | —         | ✅ Đã có |
| Interaction   | **InteractionManager** state machine (tự viết) | —      | ✅ Đã có |
| Video capture | GStreamer + OpenCV                          | 1.22 / 4.6 | ✅ Đã có |
| Database      | SQLite3 + WAL (`resident_db`, async writer) | 3.x        | ✅ Đã có |
| Migration     | `migrate_fdb` (.fdb → SQLite, 1 chiều)      | —         | ✅ Đã có |
| Logging       | **Self-written logger** (không dùng spdlog) | —         | ✅ Đã có |
| Display UI    | SDL2 + SDL2_ttf + SDL2_image                | 2.26+      | ⏳ Chưa làm |
| TTS           | Piper (offline)                             | latest     | ⏳ Chưa làm |
| Config        | YAML (hoặc giữ CLI args — xem ghi chú)      | —         | ⏳ Chưa làm |
| REST API      | cpp-httplib                                 | —         | ⏳ Chưa làm |
| Metrics       | prometheus-cpp                              | —         | ⏳ Chưa làm |
| Service       | systemd                                     | —         | ⏳ Chưa làm |
| Build         | Makefile                                    | —         | ✅ Đã có |

### 3.1 Quyết định: Logger tự viết thay vì spdlog

| Tiêu chí | spdlog | Logger tự viết (đã chọn) |
|---|---|---|
| Dependency | Thêm lib/submodule, cần setup trên arm64 | Không dependency ngoài C++17 std + POSIX |
| Build trên Orange Pi | Phức tạp hơn (cross-compile / apt) | Compile thẳng cùng Makefile, đã verify |
| Tính năng | Rất phong phú (async, backtrace, fmt) | Đủ: level, thread-safe, stderr+file, rotation/ngày, retention, màu TTY |
| Hiệu năng | Async queue tối ưu cao | fflush/record (INFO+ thưa nên OK); bản ghi bị lọc gần miễn phí |
| Bảo trì | Community | Tự duy trì (~350 dòng, đơn giản) |

**Kết luận**: dự án nhúng 1 app / 1 dev → logger tự viết ít ma sát build, không dependency, đủ tính năng
vận hành 24/7. spdlog chỉ đáng dùng khi cần multi-sink phức tạp (syslog remote, Loki) hoặc throughput
log cực cao — chưa cần ở v1. Chi tiết design: `.kiro/specs/system-logging/`.

### 3.2 Ghi chú Config YAML

Hiện tại app + tool cấu hình qua **CLI args + env** (`--resident-db`, `--cabin-id`, `--match-thr`,
`--confirm-streak`, `--cooldown-ms`, `--log-level`, `--log-dir`, `FACE_CABIN_LOG_*`...). YAML config
(`config.yaml`) vẫn trong plan nhưng ưu tiên thấp — chỉ cần khi số tham số/camera tăng khó quản qua CLI.

---

## 4. Lịch triển khai chi tiết (điều chỉnh cho 1 dev)

> Timeline gốc 6 tuần chia 2 dev. Thực tế 1 dev nên giãn thành ~8 tuần. Foundation + data layer +
> logging đã xong sớm; phần UI/TTS/ops dồn về sau.

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

- [ ] RTSP reconnect logic trong `capture_worker`: exponential backoff (chưa có code)
- [ ] Config YAML file `config.yaml`: camera URLs, models, DB path, thresholds (ưu tiên thấp, xem §3.2)
- [ ] systemd service `face-cabin.service` với `Restart=always` (chưa có)
- [ ] Tool `bulk_enroll`: CSV + `--photos-dir` → SQLite (spec format có ở `docs/BULK_ENROLL_FORMAT.md`, chưa code)
- [ ] SDL2 skeleton `src/cabin_ui.{h,cpp}`: fullscreen 1024×600, font VN UTF-8, 3 màn IDLE/DETECTING/MATCHED (chưa có)
- [ ] Touchscreen tap handler (Waveshare WS170120) (chưa có)

**Deliverables Giai đoạn 1:**

- ✅ SQLite DB enroll được từ ảnh thẻ; multi-embedding matching + audit log hoạt động
- ✅ Logging file rotation/ngày chạy, không PII
- ⏳ SDL2 idle screen trên Waveshare (chưa)
- ⏳ face_recog_app chạy 24h qua systemd (chưa có service)
- ⏳ RTSP mất kết nối tự reconnect <10s (chưa)

---

### 🗓 Giai đoạn 2 (~8/9 – 21/9) — Recognition core hoàn chỉnh + Greeting + UI base

**Mục tiêu**: E2E: bước vào cabin → chào tên (TTS + màn hình) → gọi tầng (mock).

- [X] Multi-embedding matching (đã xong ở GĐ1)
- [X] Cooldown per resident (đã xong ở GĐ1 — InteractionManager)
- [X] Event logging `match_events` (đã xong ở GĐ1)
- [ ] Liveness passive: motion diff giữa 2 frame (bbox center > 3px) chống ảnh in tĩnh
  - Optional: NPU liveness model (Silent-Face-Anti-Spoofing) nếu có thời gian
- [ ] Mock elevator: `src/elevator.{h,cpp}` interface `goto_floor(int)` — backend GPIO relay OR log `[ELEVATOR] goto floor 12`
- [ ] Auto floor-call: khi CONFIRMED và `home_floor > 0` → gọi tầng; `home_floor <= 0` → chào tên, không auto-gọi
- [ ] SDL2 UI base + state machine: IDLE → DETECTING → MATCHED (tên + tầng + countdown 3s) → CONFIRMED → MANUAL_INPUT
- [ ] Piper TTS: voice `vi_VN-...` + `en_US-...`, cache audio theo `resident_id`, template VI/EN
- [ ] Tap-to-cancel/change trên touchscreen

**Deliverables:**

- ✅ Person walks in → ~2s: TTS "Chào bác Nga, lên tầng 12" + relay trigger (mock)
- ✅ Tap Cancel → hủy floor, chuyển manual input
- ✅ Ảnh in không match (liveness passive filter)

---

### 🗓 Giai đoạn 3 (~22/9 – 5/10) — Multi-person + i18n + Self-supervised + Ops

**Mục tiêu**: nhiều người, song ngữ, tự cải thiện embedding, vận hành được.

- [ ] Multi-person handler: primary = bbox lớn nhất; 2+ khác tầng → TTS + relay cho tất cả
- [ ] Self-supervised capture: sau confirm (no cancel 3s) → lưu embedding `source='cabin'`, giới hạn 10/người (drop id_photo cũ trước)
- [ ] Threshold auto-adjust: nới (0.30) khi chỉ có id_photo; siết (0.40) khi ≥3 cabin embeddings
- [ ] i18n: template `lang/vi.yaml` / `lang/en.yaml`, switch theo `resident.language`
- [ ] Confirm UX: circular countdown 3s, nút Cancel/Change rõ, font ≥32px
- [ ] RTSP reconnect + heartbeat (kéo từ GĐ1)
- [ ] systemd service + `bulk_enroll` (kéo từ GĐ1)
- [ ] Backup nightly SQLite dump; retention cron `match_events` >30 ngày (VIP 90)

**Deliverables:**

- ✅ 3 người vào cabin → chào + gọi tầng khác nhau
- ✅ VI/EN theo resident
- ✅ Sau ~10 match → in-cabin embeddings được lưu
- ✅ HR bulk enroll từ CSV + ảnh

---

### 🗓 Giai đoạn 4 (~6/10 – 19/10) — REST API + Field Test 1 Cabin

**Mục tiêu**: HR tự quản lý; chạy thật 1 cabin, thu metrics, fix bug.

- [ ] REST API (`face-cabin-api` hoặc built-in): CRUD residents, bulk upload, `GET /events`, `GET /metrics` (Prometheus)
- [ ] Auth bearer token; backup; retention
- [ ] Display polish: night mode, splash logo, fade-in khi match
- [ ] Error UI: camera offline / DB error / elevator error → fallback manual
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
| Cancel/override rate              | <10% (< là tốt)  |

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

| #   | Risk                                    | Prob | Impact  | Mitigation                                              | Trạng thái |
| --- | --------------------------------------- | ---- | ------- | ------------------------------------------------------- | ---------- |
| R1  | Ảnh thẻ 3×4 không match ảnh cabin       | High | High    | Self-supervised update + threshold nới lỏng             | ⏳ (GĐ3) |
| R2  | Cư dân già không quen tap-to-cancel     | Med  | Med     | Tap area lớn, countdown 5s cho VIP, physical fallback   | ⏳ (GĐ2-3) |
| R3  | Piper TTS đọc tên VN sai                | Med  | Low-Med | Custom `greeting_name`, test 100 tên trước              | ⏳ (GĐ2) |
| R4  | Multi-person 5+ gây confusion           | Low  | Med     | Chỉ chào top-2 primary, còn lại "và các bạn"            | ⏳ (GĐ3) |
| R5  | Liveness bypass bằng video HD           | Med  | High    | Motion diff frame + optional NPU liveness model         | ⏳ (GĐ2) |
| R6  | Camera hỏng / mạng chậm                 | Med  | Med     | RTSP reconnect + heartbeat + alert                      | ⏳ (GĐ1/3) |
| R7  | GDPR/PDPA compliance                    | Low  | High    | Consent tracking + retention + right-to-delete. **Log không PII đã enforce** | 🟡 phần logging done |
| R8  | Building manager rút consent test       | Low  | High    | Backup building 2 nếu building 1 rút                    | ⏳ |
| R9  | NPU crash không recover                 | Low  | High    | Watchdog restart service; kernel crash → reboot         | ⏳ (cần systemd) |
| R10 | ID photo scan chất lượng thấp           | High | Med     | Log failures, HR chụp lại subset                        | ⏳ (cần bulk_enroll) |
| R11 | Enroll sai `--recog-dim` → cả DB unknown | Med | Med     | **Đã fix**: cross-check dim DB vs model, LOG_ERROR + thoát (P1-3) | ✅ done |
| R12 | Trùng tên/greeting_name → nhầm người    | Med  | Med     | **Đã fix**: dùng `track_id→resident_id`, không suy theo tên (P1-2) | ✅ done |

---

## 7. Câu hỏi mở

**Đã trả lời:**

- ✅ Thang máy vendor: skip (mock) · Số cư dân: 1000 · Use case: smart cabin · FAR: 1/1000
- ✅ Team: 1 dev (điều chỉnh) · Hardware: Orange Pi A733 · Timeline: giãn ~24/10
- ✅ Consent: HR handle · Enroll source: ảnh thẻ 3×4 · Language: VI + EN
- ✅ Display: Waveshare 7" HDMI touch · Voice: chỉ TTS + confirm · Network: LAN
- ✅ Logging: logger tự viết (không spdlog) · Data source: SQLite là nguồn duy nhất (.fdb chỉ để migrate)

**Còn chưa xác định:**

- ⏳ Loại brand thang máy để integrate v2 (không blocker cho v1 mock)
- ⏳ Test cabin nào của building — đã có consent chưa?
- ⏳ Backup building nếu building 1 rút consent
- ⏳ Format ảnh thẻ 3×4 (JPEG scan? PNG? kích thước px thực tế?)
- ⏳ Có cần YAML config hay giữ CLI args (xem §3.2)?

---

## 8. Progress log

### Đã hoàn thành (commit trên `main`)

| Ngày  | Hạng mục                                    | Commit / Ghi chú |
| ----- | ------------------------------------------- | ---------------- |
| 24/8  | Kickoff, schema SQLite draft, CSV spec      | `db/schema.sql`, `docs/BULK_ENROLL_FORMAT.md` |
| ~/9   | MatchEngine + ResidentDB + unit tests       | `2b9e6cb` |
| ~/9   | Spec resident-db-layer + system-logging     | `7fa2fd7`, `3f03ba2` |
| ~/9   | Logger core + rotation/retention tests      | `3c24201`, `95e1d01` |
| ~/9   | Phase 1 resident-db: migrate_fdb + main wire | `680e338`, `62a915e` |
| ~/9   | InteractionManager state machine            | `98c2e11` |
| ~/9   | Phase 2: enroll_faces + add_person → SQLite | `c3a7ce0` |
| ~/9   | Link logger vào mọi binary (Makefile)       | `d46384c` |
| ~/9   | Di trú main.cpp + tool sang logger          | `5672530`, `633064e` |
| 3/9   | Code review findings                        | `aa53d21` |
| 3/9   | Fix review P1-1..P1-3 + P2-1                 | `ab02162` |

### Đang chờ / kế tiếp

- SDL2 UI, Piper TTS, mock elevator, liveness passive (GĐ2)
- Multi-person, self-supervised, i18n, RTSP reconnect, systemd, bulk_enroll (GĐ3)
- REST API, field test (GĐ4)

### Tham chiếu

- Specs: `.kiro/specs/resident-db-layer/`, `.kiro/specs/system-logging/`
- Code review đang theo dõi: `docs/CODE_REVIEW_resident-db_logging.md` (P2-2, P2-3, P3-1..P3-6 còn mở)

---

## 9. Contacts

| Role                       | Name | Contact |
| -------------------------- | ---- | ------- |
| Developer (full-stack)     | —    | —       |
| HR (enroll data + consent) | —    | —       |
| Building Manager           | —    | —       |
| Emergency contact 24/7     | —    | —       |

---

**Cập nhật cuối**: 3/9/2026
**Owner**: —
**Version**: 2.0
