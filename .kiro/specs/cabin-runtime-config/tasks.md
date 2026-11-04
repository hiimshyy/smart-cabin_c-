# Implementation Plan: Lớp config vận hành DB-driven (cabin-runtime-config)

## Overview

**Spec ID**: `cabin-runtime-config`
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

### 📌 TRẠNG THÁI

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

## Tasks

- [x] 1. Migration schema_version 2 (`db/schema.sql` + `ResidentDB::apply_migrations`)
  - [x] 1.1 Thêm định nghĩa cột Nhóm A vào `CREATE TABLE cabins` (đường tạo-mới) + khối `ALTER TABLE ... ADD COLUMN` version 2 (đường nâng cấp). Thêm `residents.ext_id TEXT` + unique index partial. `INSERT OR IGNORE schema_version VALUES(2)`.
    - _Requirements: R1.2, R1.3, R1.4, R1.6_
  - [x] 1.2 `ResidentDB::apply_migrations()`: đọc `current_schema_version()`; nếu < 2 chạy khối ALTER trong transaction, nuốt lỗi "duplicate column", commit. Gọi từ `open()` sau `has_tables()`.
    - _Requirements: R1.1, R1.5_
  - [x] 1.3 Test migration: (a) DB version 1 cũ → mở → có đủ cột + version=2 + residents giữ nguyên; (b) DB mới từ schema.sql → không lỗi duplicate; (c) mở lại lần 2 idempotent.
    - _Requirements: R1.1, R1.5, R1.6, NFR không hồi quy_

- [x] 2. `ResidentDB::load_cabin()` + `update_cabin_config()`
  - [x] 2.1 `load_cabin(int64_t id) → CabinRow`: SELECT dòng cabins theo id; `found=false` nếu không có. Đọc cả cột v1 (camera_urls, elevator_endpoint, floors_min/max) lẫn v2.
    - _Requirements: R2.1, R2.2_
  - [x] 2.2 `update_cabin_config(id, patch)`: UPDATE các cột Nhóm A (dùng cho REST PATCH). Synchronous. Chỉ đụng cột Nhóm A.
    - _Requirements: R4.7, R5.1, R5.4_
  - [x] 2.3 Unit test round-trip: update → load lại đúng giá trị; cabin id không tồn tại → found=false.
    - _Requirements: R2.2_

- [x] 3. Module `src/cabin_config.{h,cpp}` — struct + resolve + validate
  - [x] 3.1 Định nghĩa `CabinRow`, `CabinConfig`, `CliOverrides` (design, mục Data Models).
    - _Requirements: R2, R3_
  - [x] 3.2 `resolve_cabin_config(cli, db, def)`: precedence per-field CLI>DB>default (R3.1, R3.2).
    - _Requirements: R3.1, R3.2, R3.4_
  - [x] 3.3 Validate/clamp trong resolve (design, mục Components and Interfaces → Validate/clamp): match_thr, confirm_streak, cooldown/unknown/latency ≥0, reconnect min≤max, floors min≤max. LOG_WARN khi clamp.
    - _Requirements: R4.1–R4.5_
  - [x] 3.4 Parse `camera_urls` JSON array tối giản → phần tử [0]; scheme rtsp/http/https → use_stream; rỗng/lỗi/scheme lạ → fallback USB + LOG_WARN.
    - _Requirements: R2.3, R4.6_
  - [x] 3.5 `validate_cabin_patch()` (dùng chung cho REST): trả danh sách lỗi field; reject scheme lạ / pipeline string / trường Nhóm C.
    - _Requirements: R4.6, R4.7_
  - [x] 3.6 Unit test: precedence (3 tổ hợp cli/db/default), clamp từng ràng buộc, parse camera_urls (rtsp ok / rỗng / pipeline bị reject).
    - _Requirements: R2, R3, R4_

- [x] 4. Khâu nối `main.cpp` (dùng config từ DB)
  - [x] 4.1 Khi parse CLI, set cờ `CliOverrides.<field>=true` cho mỗi tham số Nhóm A người dùng thực truyền (+ lưu giá trị).
    - _Requirements: R3.2_
  - [x] 4.2 Sau khi mở `--resident-db`: `load_cabin(cabin_id)` → `resolve_cabin_config` → dùng `CabinConfig` dựng `CamConfig` (pipeline qua `build_gst_pipeline`), `InteractionConfig`, `match_threshold`, backoff. Không đọc cabin ở nhánh `--face-db`.
    - _Requirements: R2.1, R2.3, R3.4_
  - [x] 4.3 LOG_INFO một dòng config hiệu lực + nguồn từng field (cli/db/default) lúc khởi động.
    - _Requirements: R3.3, NFR quan sát được_
  - [x] 4.4 Build trên Orange Pi (NPU/OpenCV): `make -j4` build sạch 6 binary, 0 warning mới; unit suite 129/129 pass (gồm 25 cabin_config). Smoke test chạy-thật (a) DB có RTSP → app mở RTSP; (b) CLI `--source` override DB; (c) cabin id thiếu → default + warn — GỘP vào đợt test camera pending (cùng RTSP reconnect + refactor runtime diff); logic resolve đã được unit test bao phủ đầy đủ.
    - _Requirements: R2.1, R2.2, R3.1_

- [ ] 5. REST API config (thuộc spec Enroll API — làm cùng lúc REST layer lên) — HOÃN
       Backend đã sẵn sàng: `validate_cabin_patch()` + `ResidentDB::update_cabin_config()` /
       `load_cabin()` đã implement + test. Chỉ còn phần HTTP endpoint, gộp vào đợt dựng REST
       layer của spec Enroll API (Ưu tiên 1) — không dựng server riêng.
  - [ ] 5.1 `GET /api/v1/cabins/{id}`: trả JSON trường Nhóm A (không lộ Nhóm C).
    - _Requirements: R6 (design, mục REST API cho config)_
  - [ ] 5.2 `PATCH /api/v1/cabins/{id}`: body JSON → `validate_cabin_patch` → `update_cabin_config`; response `{ok, restart_required:true, cabin}`. Trường Nhóm C / URL lỗi → 400.
    - _Requirements: R4.6, R4.7, R5.1, R5.2_
  - [ ] 5.3 Test API: PATCH camera_urls hợp lệ → DB đổi + restart_required; PATCH model path → 400; PATCH pipeline string → 400.
    - _Requirements: R4.6, R4.7_

- [x] 6. Tài liệu vận hành
  - [x] 6.1 README: bảng "web chỉnh được gì" (Nhóm A) + ghi chú "đổi RTSP cần restart thiết bị".
  - [x] 6.2 Cập nhật `env.sh` help nếu cần (ghi rõ CLI giờ override DB).
  - [x] 6.3 DEVELOPMENT_PLAN §3.2: thay ghi chú YAML bằng link spec này (đã làm ở task list tài liệu).
    - _Requirements: R5.5, NFR quan sát được_

## Notes

### Thứ tự đề xuất

Task 1–3 (migration + module config + test) không phụ thuộc NPU → làm + test trước bằng g++ +
sqlite3 ngay trên Orange Pi (nhanh, không cần load model). Task 4 build đầy đủ bằng `make` trên
Orange Pi (NPU/OpenCV) + smoke test tại chỗ. Task 5 gộp vào đợt dựng REST layer của spec Enroll API
không dựng server riêng cho config. Task 6 chốt cuối.

### Về Task Dependency Graph

- Graph ghi phụ thuộc theo luồng dữ liệu trong `design.md`: migration runtime (1.2) chạy khối ALTER của schema version 2 (1.1); `load_cabin`/`update_cabin_config` (2.1, 2.2) cần cột v2 (1.2) và struct `CabinRow` (3.1, đặt trong `src/resident_db.h`); `resolve_cabin_config` (3.2) dùng struct của 3.1; clamp (3.3) và parse `camera_urls` (3.4) nằm trong resolve; `validate_cabin_patch` (3.5) dùng chung luật URL của 3.4.
- Task 4.x đặt sau khi task 1–3 đã qua test (1.3, 2.3, 3.6), theo "Thứ tự đề xuất" ở trên. Về mặt code, 4.1 chỉ cần `CliOverrides` (3.1).
- Task 5.x chỉ phụ thuộc backend trong spec này (2.3, 3.6). Ngoài graph, task 5 còn bị chặn bởi REST layer của spec Enroll API.
- Task 6.x đặt sau 4.4 vì tài liệu mô tả hành vi đã nối vào `main.cpp` và `env.sh` (CLI override DB). Task 6 không chờ task 5: task 5 đang hoãn, task 6 đã xong.
- Theo `design.md` (mục "Khoảng trống đã biết"), hiện chưa có test nào trong `tests/` gọi `load_cabin`, `update_cabin_config` hoặc chạy migration v1→v2, dù task 1.3 và 2.3 đã đánh dấu xong.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "3.1"] },
    { "id": 1, "tasks": ["1.2", "3.2"] },
    { "id": 2, "tasks": ["1.3", "2.1", "2.2", "3.3", "3.4"] },
    { "id": 3, "tasks": ["2.3", "3.5"] },
    { "id": 4, "tasks": ["3.6", "5.1"] },
    { "id": 5, "tasks": ["4.1", "5.2"] },
    { "id": 6, "tasks": ["4.2", "5.3"] },
    { "id": 7, "tasks": ["4.3"] },
    { "id": 8, "tasks": ["4.4"] },
    { "id": 9, "tasks": ["6.1", "6.2", "6.3"] }
  ]
}
```
