# Smart Elevator Cabin — Kế hoạch phát triển

**Dự án**: Face Recognition Smart Cabin for Elevator
**Timeline**: 25/8/2026 → 10/10/2026 (6 tuần + go-live)
**Hardware**: Orange Pi A733 (NPU) + Waveshare 7" 1024×600 HDMI touchscreen
**Target**: 2 cabins, ~1000 residents, FAR 1/1000

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

```
     ┌──────────────┐     ┌──────────────┐
     │  Camera 1    │     │  Camera 2    │  RTSP LAN
     └──────┬───────┘     └──────┬───────┘
            └──────┬──────────────┘
                   ▼
            ┌─────────────────┐
            │  Vision daemon  │  NPU: detect + recog + liveness
            │  (face_recog_app)│
            └────────┬────────┘
                     │
      ┌──────────────┼──────────────┬──────────────┐
      ▼              ▼              ▼              ▼
  ┌───────┐  ┌──────────────┐  ┌─────────┐  ┌───────────┐
  │ Piper │  │  Display     │  │ Elev.   │  │  SQLite   │
  │ TTS   │  │  Manager     │  │ Mock    │  │  residents│
  │ VI+EN │  │  SDL2 7"     │  │ GPIO/log│  │  + events │
  └───────┘  └──────────────┘  └─────────┘  └───────────┘
```

**Data flow per interaction** (target < 2s):

```
frame → detect (19ms) → align (1ms) → recog (3ms) →
  liveness (10ms) → DB match (0.5ms) → TTS+Display+Relay
```

---

## 3. Tech Stack

| Layer         | Library                               | Version    | Trạng thái                          |
| ------------- | ------------------------------------- | ---------- | ------------------------------------- |
| Detect/Recog  | awnn_lib + Retinaface + MobileFaceNet | current    | ✅ Đã có                           |
| Video capture | GStreamer + OpenCV                    | 1.22 / 4.6 | ✅ Đã có                           |
| Database      | SQLite3 + WAL                         | 3.x        | ⏳ Cần`libsqlite3-dev`             |
| Display UI    | SDL2 + SDL2_ttf + SDL2_image          | 2.26+      | ⏳ Cần`libsdl2-*-dev`              |
| TTS           | Piper (offline)                       | latest     | ⏳ Tuần 2                            |
| Config        | YAML                                  | —         | ⏳ Tuần 1                            |
| Logging       | spdlog                                | —         | ⏳ Tuần 1                            |
| REST API      | cpp-httplib                           | —         | ⏳ Tuần 4                            |
| Metrics       | prometheus-cpp                        | —         | ⏳ Tuần 4                            |
| Service       | systemd                               | —         | ⏳ Tuần 1                            |
| Build         | Makefile                              | —         | ✅ Đã có (upgrade CMake nếu cần) |

---

## 4. Lịch triển khai chi tiết

### 🗓 Tuần 1 (25/8 – 31/8) — Foundation

**Mục tiêu**: hệ thống nền tảng vững, chuyển sang SQLite, chạy 24/7.

#### Dev A (backend)

- [ ] Cài dev libs: `libsqlite3-dev`, `libsdl2-*-dev`
- [ ] Wrap SQLite: `src/resident_db.{h,cpp}` với API cơ bản
- [ ] Apply schema `db/schema.sql`, unit test CRUD
- [ ] Migration script `.fdb` → SQLite (đọc DB cũ, insert vào bảng embeddings)
- [ ] RTSP reconnect logic trong `capture_worker`: exponential backoff
- [ ] Config YAML file (`config.yaml`): camera URLs, models, DB path, thresholds
- [ ] systemd service `face-cabin.service` với `Restart=always`

#### Dev B (import + display base)

- [ ] Tool `bulk_enroll`: đọc CSV + `--photos-dir` → SQLite
  - Xử lý failure list → `enroll_failures.csv`
  - Support `--overwrite` / `--skip-existing`
- [ ] SDL2 skeleton app `src/cabin_ui.{h,cpp}`:
  - Fullscreen 1024×600
  - Font Vietnamese UTF-8 (Noto Sans / Roboto)
  - 3 màn hình cơ bản: `IDLE`, `DETECTING`, `MATCHED_STUB`
- [ ] Touchscreen tap event handler (WaveShare WS170120)

**Deliverables tuần 1**:

- ✅ SQLite DB có thể enroll từ 5 ảnh thẻ mẫu
- ✅ SDL2 window render idle screen trên Waveshare
- ✅ face_recog_app chạy 24h liên tục qua systemd, không crash
- ✅ RTSP mất kết nối → tự reconnect trong <10s

---

### 🗓 Tuần 2 (1/9 – 7/9) — Recognition + Greeting Core

**Mục tiêu**: E2E flow: bước vào cabin → TTS chào tên → gọi tầng (mock).

#### Dev A

- [ ] Multi-embedding matching: match với **max similarity** trong list, không phải avg
- [ ] Liveness passive: kiểm tra motion giữa 2 frame consecutive
  - Diff bbox center movement > 3px (chống ảnh in tĩnh)
  - Optional: NPU liveness model (Silent-Face-Anti-Spoofing) — nếu có thời gian
- [ ] Mock elevator: GPIO relay OR log-only `[ELEVATOR] goto floor 12`
- [ ] Cooldown 3s per resident (tránh flip-flop)
- [ ] Event logging vào bảng `match_events`

#### Dev B

- [ ] Piper TTS setup:
  - Download voice `vi_VN-25hours-single-medium` (~30MB)
  - Download voice `en_US-libritts-r-medium` (~50MB)
  - Build hoặc apt install `piper-tts`
- [ ] Template engine:
  - VI: `"Chào {greeting_name}, cabin lên tầng {home_floor} nhé!"`
  - EN: `"Hello {greeting_name}, going to floor {home_floor}."`
- [ ] Cache TTS audio theo `resident_id` (không phải regenerate mỗi lần)
- [ ] Display state machine hoàn chỉnh:
  - `IDLE` → `DETECTING` (khi có face)
  - `DETECTING` → `MATCHED` (recog OK)
  - `MATCHED`: show name + floor + 3s countdown progress
  - `CONFIRMED` → gọi relay + play TTS
  - Timeout/tap-cancel → `MANUAL_INPUT` (grid tầng 1-30)

**Deliverables tuần 2**:

- ✅ Person walks in → 2s later: TTS "Chào bác Nga, lên tầng 12" + relay trigger
- ✅ Tap "Cancel" → hủy floor, chuyển manual input
- ✅ Ảnh in không match được (liveness passive filter)

---

### 🗓 Tuần 3 (8/9 – 14/9) — Multi-person + i18n

**Mục tiêu**: xử lý nhiều người + song ngữ VI/EN.

#### Dev A

- [ ] Multi-person handler:
  - Detect nhiều faces cùng frame
  - Priority: closest-to-camera (bbox area lớn nhất) là primary
  - Nếu 2+ khác tầng: gọi TTS "Đưa {name1} tầng X, {name2} tầng Y"
  - Relay signal cho ALL selected floors
- [ ] Self-supervised embedding capture:
  - Sau khi confirm (no cancel trong 3s) → save embedding mới vào `embeddings` với `source='cabin'`
  - Auto-limit 10 embeddings per person (drop oldest ID photo trước)
- [ ] Threshold auto-adjustment:
  - Nới lỏng (0.30) khi resident chỉ có ID photo embedding
  - Siết (0.40) khi resident có ≥3 cabin embeddings

#### Dev B

- [ ] i18n system:
  - Load string templates từ file `lang/vi.yaml`, `lang/en.yaml`
  - Language switch based on `resident.language`
- [ ] Confirm UX polish:
  - Circular countdown progress (3s)
  - Cancel/Change floor buttons rõ ràng
  - Font size ≥ 32px cho người già
- [ ] Empty state: cabin trống → clock + welcome message

**Deliverables tuần 3**:

- ✅ 3 người vào cabin → chào 3 người → gọi 3 tầng (nếu khác nhau)
- ✅ VI resident thấy tiếng Việt, EN resident thấy tiếng Anh
- ✅ Sau 10 lần match → in-cabin embeddings được lưu

---

### 🗓 Tuần 4 (15/9 – 21/9) — REST API + Ops

**Mục tiêu**: HR tự quản lý được cư dân, ops có metrics + logs.

#### Dev A

- [ ] REST API server (`face-cabin-api` binary hoặc built-in):
  - `POST /api/residents/bulk` — upload CSV + ZIP ảnh
  - `POST /api/residents` — thêm 1 (multipart: JSON + photo)
  - `PUT /api/residents/{id}` — update fields
  - `DELETE /api/residents/{id}` — soft delete (set `active=0`)
  - `GET /api/residents` — list, filter
  - `GET /api/events?since=...&resident_id=...` — audit
  - `GET /api/metrics` — Prometheus format
- [ ] Auth: simple bearer token (config file)
- [ ] Backup: nightly SQLite dump → `/var/backups/face-cabin/YYYY-MM-DD.sql.gz`
- [ ] Retention: cron xóa `match_events` > 30 ngày (VIP: 90)

#### Dev B

- [ ] Display polish:
  - Night mode (giảm brightness sau 22h)
  - Splash screen với logo building
  - Animation nhẹ khi match (fade in name)
- [ ] Error UI states:
  - Camera offline → biểu tượng warning
  - DB error → "System offline, use manual"
  - Elevator error → "Please use buttons manually"
- [ ] Emergency mode: 3 miss liên tiếp → auto-fallback to manual + log incident

**Deliverables tuần 4**:

- ✅ HR upload CSV + ZIP → 1000 residents enroll trong <10 phút
- ✅ Grafana dashboard: FPS, match rate, camera uptime, latency P95
- ✅ Log rotation setup, backup định kỳ

---

### 🗓 Tuần 5 (22/9 – 28/9) — Field Test 1 Cabin

**Mục tiêu**: chạy thật 1 tuần, thu thập metrics, fix bugs.

#### Cả 2 dev

- [ ] Cài đặt trên 1 thang máy building (đã có consent BQL)
- [ ] Enroll 20-50 volunteers từ ID photos
- [ ] Daily standup 15 min triage
- [ ] Live monitoring dashboard
- [ ] Consent poster + fallback poster in cabin
- [ ] Emergency contact 24/7 setup

**Metrics theo dõi (mục tiêu)**:

| Metric                            | Target            |
| --------------------------------- | ----------------- |
| Match rate (registered residents) | >90%              |
| False positive rate               | <0.1%             |
| E2E latency P95                   | <2s               |
| Voice greeting completion         | >95%              |
| System uptime                     | >99%              |
| Cancel/override rate              | <10% (< là tốt) |

**Deliverables tuần 5**:

- ✅ 100+ recognition events log
- ✅ FAR đo empirical <0.1%
- ✅ Bug list được fix rolling
- ✅ Feedback từ volunteers → adjust threshold, greeting text

---

### 🗓 Tuần 6 (29/9 – 5/10) — Deploy 2 Cabins + Go-Live

**Mục tiêu**: 2 cabins production, training staff.

- [ ] Roll out cabin thứ 2
- [ ] Migrate embeddings (self-supervised) từ cabin 1 → shared DB
- [ ] Training staff BQL: dùng REST API để add/remove residents
- [ ] Documentation:
  - Ops runbook (`docs/RUNBOOK.md`): restart, troubleshoot, backup/restore
  - Resident FAQ card (in cabin): "Hệ thống này là gì? Tôi có quyền gì?"
  - API doc (`docs/API.md`): endpoint reference
- [ ] Load test: bench với 10 fake concurrent recognitions
- [ ] Final security review: no plaintext credentials, no PII in logs

**Deliverables tuần 6**:

- ✅ 2 cabins live
- ✅ Enroll 1000 residents (nếu ID photos đủ)
- ✅ Runbook + FAQ hoàn chỉnh
- ✅ SLA đạt: 99.5% uptime, <2s latency, FAR <0.1%

---

### 🚀 Go-Live 10/10/2026

- Formal announcement to residents (with 1 tuần trước)
- Support hotline 24/7 first week
- Monitor daily → fix bug rolling
- Retrospective sau 1 tháng

---

## 5. Data schema tham chiếu

Xem chi tiết trong `db/schema.sql`.

**Bảng chính**:

- `residents` — 1000 rows target
- `embeddings` — 3000-10000 rows (multi-embedding per person)
- `cabins` — 2 rows
- `match_events` — grow ~100-500 rows/day, retention 30 ngày
- `schema_version` — migration tracking

---

## 6. Risk Register

| #   | Risk                                           | Probability | Impact  | Mitigation                                              | Owner      |
| --- | ---------------------------------------------- | ----------- | ------- | ------------------------------------------------------- | ---------- |
| R1  | Ảnh thẻ 3×4 không match được ảnh cabin | High        | High    | Self-supervised update + threshold nới lỏng           | Dev A      |
| R2  | Cư dân già không quen tap-to-cancel        | Med         | Med     | Tap area lớn, countdown 5s cho VIP, physical fallback  | Dev B      |
| R3  | Piper TTS đọc tên VN sai                    | Med         | Low-Med | Custom`greeting_name` field, test 100 tên trước    | Dev B      |
| R4  | Multi-person 5+ gây confusion                 | Low         | Med     | Chỉ chào top-2 primary, còn lại "và các bạn"     | Dev A      |
| R5  | Liveness bypass bằng video HD                 | Med         | High    | Motion diff frame + optional NPU liveness model         | Dev A      |
| R6  | Camera hỏng / mạng chậm                     | Med         | Med     | Reconnect + heartbeat + alert                           | Dev A      |
| R7  | GDPR/PDPA compliance                           | Low         | High    | Consent tracking + retention + right-to-delete          | Dev A      |
| R8  | Building manager rút consent test             | Low         | High    | Backup building 2 nếu building 1 rút                  | PM         |
| R9  | NPU crash không recover                       | Low         | High    | Watchdog restart service; kernel driver crash → reboot | Dev A      |
| R10 | ID photo scan chất lượng thấp              | High        | Med     | Log failures, HR chụp lại subset                      | HR + Dev B |

---

## 7. Câu hỏi mở

Đã trả lời:

- ✅ Thang máy vendor: skip (mock)
- ✅ Số cư dân: 1000
- ✅ Use case: smart cabin
- ✅ FAR target: 1/1000
- ✅ Team: 2 devs
- ✅ Hardware: Orange Pi (upgrade Jetson nếu cần)
- ✅ Timeline: 10/10
- ✅ Building manager consent: HR handle
- ✅ Enroll source: ảnh thẻ 3×4
- ✅ Language: VI + EN
- ✅ Display: Waveshare 7" HDMI touch
- ✅ Voice: chỉ TTS + confirm
- ✅ Network: LAN

Còn chưa xác định:

- ⏳ Loại brand thang máy để integrate v2 (không blocker cho v1 mock)
- ⏳ Test cabin nào của building — đã có consent chưa?
- ⏳ Backup building nếu building 1 rút consent
- ⏳ Format ảnh thẻ 3×4 (JPEG scan? PNG? kích thước px thực tế?)

---

## 8. Progress log

### Tuần 1 tracking

| Ngày | Dev | Task                            | Status         | Notes                          |
| ----- | --- | ------------------------------- | -------------- | ------------------------------ |
| 24/8  | —  | Kickoff, requirements gather    | ✅ Done        | Full scope confirmed           |
| 24/8  | A   | Schema SQLite draft             | ✅ Done        | `db/schema.sql`              |
| 24/8  | B   | CSV format spec                 | ✅ Done        | `docs/BULK_ENROLL_FORMAT.md` |
| 25/8  | A   | Install dev libs                | ⏳ In progress | Waiting for sudo               |
| 25/8  | A   | resident_db.{h,cpp} wrap SQLite | 📋 Pending     |                                |
| 25/8  | B   | bulk_enroll tool                | 📋 Pending     |                                |
| 25/8  | B   | SDL2 skeleton                   | 📋 Pending     |                                |
| ...   |     |                                 |                |                                |

### Tuần 2 tracking

(Sẽ update sau khi hoàn thành Tuần 1)

### ...

---

## 9. Contacts

| Role                       | Name | Contact |
| -------------------------- | ---- | ------- |
| Dev A (backend + vision)   | —   | —      |
| Dev B (frontend + voice)   | —   | —      |
| HR (enroll data + consent) | —   | —      |
| Building Manager           | —   | —      |
| Emergency contact 24/7     | —   | —      |

---

**Cập nhật cuối**: 24/8/2026
**Owner**: —
**Version**: 1.0
