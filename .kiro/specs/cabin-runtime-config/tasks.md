# Tasks — Lớp config vận hành DB-driven (cabin-runtime-config)

**Spec ID**: `cabin-runtime-config`

---

## 📌 TRẠNG THÁI

**Chưa bắt đầu code.** Tài liệu (requirements + design) đã chốt. Đây là spec Đề xuất 4, phụ thuộc
`resident-db-layer` (đã xong) và dùng chung REST layer với spec Enroll API (đang là Ưu tiên 1).

**Quyết định đã chốt (để không hỏi lại):**
- Config vận hành: **DB-driven** (bảng `cabins`), thay cho ý tưởng `config.yaml` ở DEVELOPMENT_PLAN §3.2.
- Precedence **CLI > DB > default**; cần `CliOverrides` cờ per-field để phân biệt "không truyền" vs "truyền trùng default".
- Đổi RTSP/config khi đang chạy = **restart-to-apply** (systemd `Restart=always`), KHÔNG hot-reload ở v1.
- Web chỉ chỉnh **Nhóm A** (RTSP, threshold, cooldown, ...); Nhóm C (model path, log, gst-pipeline) **cấm** — REST trả 400.
- `camera_urls` chỉ nhận URL (rtsp/http/https); chuỗi GStreamer pipeline tùy ý bị từ chối (chống injection).
- Migration gộp `residents.ext_id` cùng version 2 (DEVELOPMENT_PLAN §3.5) để migrate 1 lần.

**Ghi chú build/test:** toàn bộ công việc chạy **trực tiếp trên Orange Pi** (Linux, có đủ g++ +
sqlite3 + NPU/OpenCV/AI SDK). Module `src/cabin_config.{h,cpp}` không phụ thuộc NPU nên có thể test
độc lập bằng g++ + sqlite3 (nhanh, không cần load model), giống các module `resident-db-layer`;
migration test bằng DB tạm. Khâu nối `main.cpp` build đầy đủ bằng `make` trên chính Orange Pi để xác
nhận (NPU/OpenCV). Không có ràng buộc "blocked-pending-Linux" — mọi task đều verify được tại chỗ.

---

- [ ] 1. Migration schema_version 2 (`db/schema.sql` + `ResidentDB::apply_migrations`)
  - [ ] 1.1 Thêm định nghĩa cột Nhóm A vào `CREATE TABLE cabins` (đường tạo-mới) + khối `ALTER TABLE ... ADD COLUMN` version 2 (đường nâng cấp). Thêm `residents.ext_id TEXT` + unique index partial. `INSERT OR IGNORE schema_version VALUES(2)`.
    - _Requirements: R1.2, R1.3, R1.4, R1.6_
  - [ ] 1.2 `ResidentDB::apply_migrations()`: đọc `current_schema_version()`; nếu < 2 chạy khối ALTER trong transaction, nuốt lỗi "duplicate column", commit. Gọi từ `open()` sau `has_tables()`.
    - _Requirements: R1.1, R1.5_
  - [ ] 1.3 Test migration: (a) DB version 1 cũ → mở → có đủ cột + version=2 + residents giữ nguyên; (b) DB mới từ schema.sql → không lỗi duplicate; (c) mở lại lần 2 idempotent.
    - _Requirements: R1.1, R1.5, R1.6, NFR không hồi quy_

- [ ] 2. `ResidentDB::load_cabin()` + `update_cabin_config()`
  - [ ] 2.1 `load_cabin(int64_t id) → CabinRow`: SELECT dòng cabins theo id; `found=false` nếu không có. Đọc cả cột v1 (camera_urls, elevator_endpoint, floors_min/max) lẫn v2.
    - _Requirements: R2.1, R2.2_
  - [ ] 2.2 `update_cabin_config(id, patch)`: UPDATE các cột Nhóm A (dùng cho REST PATCH). Synchronous. Chỉ đụng cột Nhóm A.
    - _Requirements: R4.7, R5.1, R5.4_
  - [ ] 2.3 Unit test round-trip: update → load lại đúng giá trị; cabin id không tồn tại → found=false.
    - _Requirements: R2.2_

- [ ] 3. Module `src/cabin_config.{h,cpp}` — struct + resolve + validate
  - [ ] 3.1 Định nghĩa `CabinRow`, `CabinConfig`, `CliOverrides` (design §4).
    - _Requirements: R2, R3_
  - [ ] 3.2 `resolve_cabin_config(cli, db, def)`: precedence per-field CLI>DB>default (R3.1, R3.2).
    - _Requirements: R3.1, R3.2, R3.4_
  - [ ] 3.3 Validate/clamp trong resolve (design §4.2): match_thr, confirm_streak, cooldown/unknown/latency ≥0, reconnect min≤max, floors min≤max. LOG_WARN khi clamp.
    - _Requirements: R4.1–R4.5_
  - [ ] 3.4 Parse `camera_urls` JSON array tối giản → phần tử [0]; scheme rtsp/http/https → use_stream; rỗng/lỗi/scheme lạ → fallback USB + LOG_WARN.
    - _Requirements: R2.3, R4.6_
  - [ ] 3.5 `validate_cabin_patch()` (dùng chung cho REST): trả danh sách lỗi field; reject scheme lạ / pipeline string / trường Nhóm C.
    - _Requirements: R4.6, R4.7_
  - [ ] 3.6 Unit test: precedence (3 tổ hợp cli/db/default), clamp từng ràng buộc, parse camera_urls (rtsp ok / rỗng / pipeline bị reject).
    - _Requirements: R2, R3, R4_

- [ ] 4. Khâu nối `main.cpp` (dùng config từ DB)
  - [ ] 4.1 Khi parse CLI, set cờ `CliOverrides.<field>=true` cho mỗi tham số Nhóm A người dùng thực truyền (+ lưu giá trị).
    - _Requirements: R3.2_
  - [ ] 4.2 Sau khi mở `--resident-db`: `load_cabin(cabin_id)` → `resolve_cabin_config` → dùng `CabinConfig` dựng `CamConfig` (pipeline qua `build_gst_pipeline`), `InteractionConfig`, `match_threshold`, backoff. Không đọc cabin ở nhánh `--face-db`.
    - _Requirements: R2.1, R2.3, R3.4_
  - [ ] 4.3 LOG_INFO một dòng config hiệu lực + nguồn từng field (cli/db/default) lúc khởi động.
    - _Requirements: R3.3, NFR quan sát được_
  - [ ] 4.4 Build trên Orange Pi (NPU/OpenCV) + smoke test: (a) DB có RTSP → app mở RTSP đó; (b) CLI `--source` override DB; (c) cabin id thiếu → default + warn, không crash.
    - _Requirements: R2.1, R2.2, R3.1_

- [ ] 5. REST API config (thuộc spec Enroll API — làm cùng lúc REST layer lên)
  - [ ] 5.1 `GET /api/v1/cabins/{id}`: trả JSON trường Nhóm A (không lộ Nhóm C).
    - _Requirements: R6 (design §6)_
  - [ ] 5.2 `PATCH /api/v1/cabins/{id}`: body JSON → `validate_cabin_patch` → `update_cabin_config`; response `{ok, restart_required:true, cabin}`. Trường Nhóm C / URL lỗi → 400.
    - _Requirements: R4.6, R4.7, R5.1, R5.2_
  - [ ] 5.3 Test API: PATCH camera_urls hợp lệ → DB đổi + restart_required; PATCH model path → 400; PATCH pipeline string → 400.
    - _Requirements: R4.6, R4.7_

- [ ] 6. Tài liệu vận hành
  - [ ] 6.1 README: bảng "web chỉnh được gì" (Nhóm A) + ghi chú "đổi RTSP cần restart thiết bị".
  - [ ] 6.2 Cập nhật `env.sh` help nếu cần (ghi rõ CLI giờ override DB).
  - [ ] 6.3 DEVELOPMENT_PLAN §3.2: thay ghi chú YAML bằng link spec này (đã làm ở task list tài liệu).
    - _Requirements: R5.5, NFR quan sát được_

---

## Thứ tự đề xuất

Task 1–3 (migration + module config + test) không phụ thuộc NPU → làm + test trước bằng g++ +
sqlite3 ngay trên Orange Pi (nhanh, không cần load model). Task 4 build đầy đủ bằng `make` trên
Orange Pi (NPU/OpenCV) + smoke test tại chỗ. Task 5 gộp vào đợt dựng REST layer của spec Enroll API
không dựng server riêng cho config. Task 6 chốt cuối.
