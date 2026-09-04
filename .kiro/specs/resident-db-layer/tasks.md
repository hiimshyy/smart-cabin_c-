# Tasks — Lớp SQLite + Multi-embedding + State Machine tương tác

**Spec ID**: `resident-db-layer`

---

## 📌 TRẠNG THÁI (cập nhật để tiếp tục trên Orange Pi)

**Đã xong (commit `2b9e6cb`):**
- ✅ **Task 2** — `MatchEngine` (`src/match_engine.h/.cpp`) + unit test `tests/test_match_engine.cpp` (14 checks pass).
- ✅ **Task 3** — `ResidentDB` (`src/resident_db.h/.cpp`) + unit test `tests/test_resident_db.cpp` (22 checks pass).
- ✅ **Task 8** (Phase 2 API, làm sớm) — `upsert_resident` / `add_embedding` / `delete_embeddings` đã có trong `ResidentDB` và đã test round-trip.

**Đã xong (commit `62a915e` trên Orange Pi):**
- ✅ **Task 1** — `Makefile`: thêm nhóm `DB_SRCS` (`resident_db.cpp` + `match_engine.cpp` + `interaction.cpp`) vào `APP_SRCS_CPP`; `-lsqlite3` vào `LIBS`. Sửa `src/match_engine.h` thiếu `#include <cstddef>` (g++ 12.2 trên Orange Pi chặt hơn). Build sạch cả 5 binary; `ldd` xác nhận `libsqlite3.so.0` link vào `face_recog_app`.

**Đã xong (chưa commit — đang trong working tree):**
- ✅ **Task 5** — `InteractionManager` (`src/interaction.h/.cpp`) state machine DETECTING→MATCHED→CONFIRMED + cooldown per-resident + unknown timeout. Unit test `tests/test_interaction.cpp` (48 checks pass, 9 scenarios). `interaction.cpp` đã thêm vào `DB_SRCS`; `nm` xác nhận symbols link vào `face_recog_app`. Tổng test hiện tại: **84 checks pass** (14 + 48 + 22).

**Chưa xong — cần làm tiếp trên Orange Pi (theo thứ tự đề xuất):**
1. **Task 4** — tool `migrate_fdb` + target Makefile.
2. **Task 6** — khâu nối vào `main.cpp` (cờ CLI + vòng lặp + cleanup).
3. **Task 7** — test tích hợp trên Orange Pi (cần camera + NPU).
4. **Task 9, 10, 11** — Giai đoạn 2 (chuyển `enroll_faces`/`add_person`).

**Ghi chú build/test trên Orange Pi:**
- `libsqlite3-dev` đã cài (v3.40.1). Chạy unit test: `bash tests/run_tests.sh` (dùng system sqlite).
- `MatchEngine`/`ResidentDB`/`InteractionManager` KHÔNG cần NPU → test được ở bất kỳ máy nào có g++ + sqlite.
- `run_tests.sh` fallback no-sudo sqlite qua `tests/_setup_sqlite_local.sh` → `/tmp/sqlite_local` (cho máy dev không có sudo).
- `ResidentDB` dùng `<thread>` → Makefile đã có sẵn `-lpthread`.

**Quyết định thiết kế đã chốt (để không phải hỏi lại):**
- `home_floor = 0` (hằng `HOME_FLOOR_UNSET`) = "chưa đăng ký tầng"; cabin chào tên nhưng không auto-gọi tầng.
- SQLite là nguồn dữ liệu DUY NHẤT cho vận hành; `.fdb` chỉ còn là input cho `migrate_fdb` + chế độ test qua `--face-db`.
- `MatchEngine` KHÔNG gộp trung bình embedding — giữ tất cả, lấy max cosine.
- `InteractionManager`: `subject_key` = `track_id` khi có tracker, ngược lại = slot ổn định (largest face). Cooldown per-resident (không per-subject) → người rời rồi quay lại trong cooldown không retrigger.

---

- [x] 1. Cập nhật build cho SQLite  ✅ (commit `62a915e`)
  - [x] Thêm `-lsqlite3` vào `LIBS` trong Makefile; nhóm `DB_SRCS` (`resident_db.cpp` + `match_engine.cpp` + `interaction.cpp`) vào `APP_SRCS_CPP`. `libsqlite3-dev` v3.40.1 đã cài trên Orange Pi.
  - [x] Sửa `src/match_engine.h` thiếu `#include <cstddef>` (g++ 12.2 Orange Pi chặt hơn máy dev). Build sạch cả 5 binary; `ldd face_recog_app` có `libsqlite3.so.0`.
  - _Requirements: R5, NFR build_

- [x] 2. `MatchEngine` (multi-embedding) + unit test  ✅
  - [x] 2.1 Tạo `src/match_engine.h/.cpp`: `MatchResult`, class `MatchEngine` (flat array, owner map), `build()`/`build_raw()`/`match()` cosine max/`vector_count()`.
    - _Requirements: R2.1, R2.2, R2.3, R2.5_
  - [x] 2.2 Unit test (máy dev): `tests/test_match_engine.cpp` — 14 checks pass (max cosine, dưới ngưỡng → unknown, empty, dim mismatch).
    - _Requirements: R2.2, R2.3_

- [x] 3. `ResidentDB` (wrapper SQLite) + unit test  ✅
  - [x] 3.1 `src/resident_db.h/.cpp`: `open()` (WAL, foreign_keys, busy_timeout, apply `db/schema.sql` nếu trống).
    - _Requirements: R1.1, R1.2, R1.6_
  - [x] 3.2 `load_active()` đọc residents active=1 + embeddings (blob→vector); struct `Resident`/`EmbeddingRow`/`MatchEvent`.
    - _Requirements: R1.3, R2.1_
  - [x] 3.3 Async event writer: `log_event()`/`touch_resident()` push queue; thread nền batch mỗi 500ms trong 1 transaction (BEGIN IMMEDIATE/COMMIT).
    - _Requirements: R1.4, R1.5, NFR non-blocking_
  - [x] 3.4 `close()` flush + join writer thread (gọi cả trong destructor).
    - _Requirements: R1.4_
  - [x] 3.5 Unit test: `tests/test_resident_db.cpp` — 22 checks pass (open tạo schema; round-trip; sentinel 0; delete; log_event kể cả NULL resident_id; touch tăng count sau flush).
    - _Requirements: R1.2, R1.3, R1.4, R1.5_

- [ ] 4. Tool `migrate_fdb` (R3)
  - [ ] 4.1 `src/migrate_fdb.cpp`: đọc `.fdb` → insert residents (`home_floor = 0` sentinel, req §6) + embeddings(source='id_photo'); trùng name skip mặc định, `--overwrite` thay thế; log tổng số resident chưa có tầng.
    - _Requirements: R3.1, R3.2, R3.3, §6_
  - [ ] 4.2 Target `migrate_fdb` vào Makefile (không cần SDK NPU); in tổng kết import/skip.
    - _Requirements: R3.4, NFR build_
  - [ ] 4.3 Test: fdb mẫu → SQLite đúng số residents/embeddings; chạy lại skip/overwrite đúng.
    - _Requirements: R3.1-R3.4_

- [x] 5. `InteractionManager` (state machine) + unit test  ✅ (working tree, chưa commit)
  - [x] 5.1 `src/interaction.h/.cpp`: `SessionState` (DETECTING/MATCHED/CONFIRMED), `Session`, `InteractionConfig` (confirm_streak=5, cooldown_ms=3000, unknown_after_ms=2000), `update()` trả `Outcome{confirmed|unknown}`; streak-based confirm; cooldown per-resident; unknown timeout single-shot. Thêm `SessionView` cho debug overlay + `state_name()`.
    - _Requirements: R4.1, R4.2, R4.3, R4.4_
  - [x] 5.2 Unit test: `tests/test_interaction.cpp` — 48 checks, 9 scenarios (empty, confirm sau đúng streak, im lặng sau confirm, cooldown chặn lần 2, cooldown hết → confirm lại, unknown 1 lần sau timeout, session drop+recreate reset timer, resident switch reset streak, 2 subject song song). `run_tests.sh` đã thêm suite này.
    - _Requirements: R4.2, R4.3, R4.4_

- [ ] 6. Khâu nối vào `main.cpp` (R5)
  - [ ] 6.1 Thêm cờ CLI `--resident-db`, `--cabin-id`, `--confirm-streak`, `--cooldown-ms`; cập nhật usage.
    - _Requirements: R5.1, R5.3_
  - [ ] 6.2 Nhánh khởi tạo khi có `--resident-db`: open DB → load_active → build MatchEngine. Giữ `--face-db` như chế độ test/dev đơn giản; usage ghi rõ "không dùng cho cabin thật".
    - _Requirements: R5.1, R5.2, R5.4, R2.1_
  - [ ] 6.3 Trong vòng lặp: thay match bằng MatchEngine, feed InteractionManager, ghi Outcome qua log_event/touch_resident; overlay hiển thị greeting_name + floor; map subject_key theo track_id nếu có tracker, ngược lại largest face.
    - _Requirements: R4.5, R5.2, R1.4, R1.5_
  - [ ] 6.4 Cleanup: `resident_db.close()` trước khi thoát.
    - _Requirements: R1.4_

- [ ] 7. Kiểm thử tích hợp Giai đoạn 1
  - [ ] 7.1 Trên Orange Pi: chạy `main --resident-db`, đi qua camera, kiểm tra `match_events` ghi đúng action/floor.
    - _Requirements: R1.4, R4.3, R4.4_
  - [ ] 7.2 Xác nhận chế độ `--face-db` (`.fdb`) vẫn chạy được cho test/dev.
    - _Requirements: R5.4_

---

### Giai đoạn 2 — Chuyển enroll tools sang SQLite (làm sau khi Giai đoạn 1 ổn)

- [x] 8. Bổ sung API ghi vào `ResidentDB`  ✅ (làm sớm cùng Task 3)
  - [x] `upsert_resident(name, home_floor=0) -> id` (trùng name trả id cũ), `add_embedding(resident_id, source, vector)`, `delete_embeddings(resident_id)`.
  - [x] Unit test round-trip cho 3 hàm (trong `tests/test_resident_db.cpp`).
  - _Requirements: R6.1, R6.2_

- [ ] 9. Chuyển `enroll_faces` sang SQLite
  - Thay `db.add()/db.save(fdb)` bằng: mỗi ảnh → `add_embedding(source='id_photo')` (không trung bình); `upsert_resident(name, home_floor=0)`. Đổi CLI `--out <fdb>` → `--db <sqlite>`. Makefile: thêm `resident_db.cpp` + `-lsqlite3`.
  - _Requirements: R6.1, R6.3_

- [ ] 10. Chuyển `add_person` sang SQLite
  - `--merge` = `add_embedding`; `--replace` = `delete_embeddings` rồi `add_embedding`; tạo resident nếu chưa có. Đổi CLI `--db <fdb>` → `--db <sqlite>`. Makefile: thêm `resident_db.cpp` + `-lsqlite3`.
  - _Requirements: R6.2, R6.3_

- [ ] 11. Kiểm thử Giai đoạn 2
  - `enroll_faces` từ folder mẫu → SQLite có đúng residents + nhiều embeddings/người; `add_person --merge/--replace` đúng. `capture_person` không đổi.
  - _Requirements: R6.1, R6.2, R6.4_
