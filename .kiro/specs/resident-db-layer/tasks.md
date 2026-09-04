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
- ✅ **Task 5** — `InteractionManager` (`src/interaction.h/.cpp`) state machine DETECTING→MATCHED→CONFIRMED + cooldown per-resident + unknown timeout. Unit test `tests/test_interaction.cpp` (48 checks pass, 9 scenarios). Đã commit `98c2e11`.
- ✅ **Task 4** — tool `migrate_fdb` (`src/migrate_fdb.cpp`) + target Makefile (link chỉ `-lsqlite3 -lpthread`, không NPU/OpenCV). Thêm `ResidentDB::find_resident()`. Test skip/overwrite/isolation OK.
- ✅ **Task 6** — khâu nối `main.cpp`: cờ `--resident-db`/`--cabin-id`/`--confirm-streak`/`--cooldown-ms`/`--unknown-after-ms`; MatchEngine + InteractionManager + log_event/touch_resident; overlay floor; `.fdb` giữ làm chế độ test/dev.
- ✅ **Task 7** — test tích hợp trên Orange Pi + camera USB (chi tiết ở checkbox Task 7).
- Tổng unit test hiện tại: **84 checks pass** (14 MatchEngine + 48 InteractionManager + 22 ResidentDB).

**Giai đoạn 1 HOÀN TẤT.** Chưa xong:
1. **Task 9, 10, 11** — Giai đoạn 2 (chuyển `enroll_faces`/`add_person` sang ghi SQLite).

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

- [x] 4. Tool `migrate_fdb` (R3)  ✅
  - [x] 4.1 `src/migrate_fdb.cpp`: đọc `.fdb` qua `FaceDB::load` → mỗi identity `upsert_resident(name, home_floor=0)` + `add_embedding(source='id_photo')`; trùng name mặc định skip+log, `--overwrite` xóa embedding cũ rồi thêm; in `imported/skipped/failed` + cảnh báo N resident `home_floor=0`. Thêm helper `ResidentDB::find_resident(name)` (h+cpp), refactor `upsert_resident` dùng nó.
    - _Requirements: R3.1, R3.2, R3.3, §6_
  - [x] 4.2 Target `migrate_fdb` vào Makefile: `MIGRATE_SRCS_CPP` = migrate_fdb + face_db + resident_db; link chỉ `-lsqlite3 -lpthread` (KHÔNG NPU/OpenCV/VIPhal, `ldd` xác nhận). Thêm vào `all` + `clean`.
    - _Requirements: R3.4, NFR build_
  - [x] 4.3 Test trên `db/faces_all.fdb` (1 identity): fresh import OK (1 resident home_floor=0, 1 emb id_photo dim=512); rerun skip đúng; `--overwrite` thay thế không tăng count; isolation test (seed resident thứ 2, `--overwrite` chỉ đụng resident khớp tên).
    - _Requirements: R3.1-R3.4_

- [x] 5. `InteractionManager` (state machine) + unit test  ✅ (commit `98c2e11`)
  - [x] 5.1 `src/interaction.h/.cpp`: `SessionState` (DETECTING/MATCHED/CONFIRMED), `Session`, `InteractionConfig` (confirm_streak=5, cooldown_ms=3000, unknown_after_ms=2000), `update()` trả `Outcome{confirmed|unknown}`; streak-based confirm; cooldown per-resident; unknown timeout single-shot. Thêm `SessionView` cho debug overlay + `state_name()`.
    - _Requirements: R4.1, R4.2, R4.3, R4.4_
  - [x] 5.2 Unit test: `tests/test_interaction.cpp` — 48 checks, 9 scenarios (empty, confirm sau đúng streak, im lặng sau confirm, cooldown chặn lần 2, cooldown hết → confirm lại, unknown 1 lần sau timeout, session drop+recreate reset timer, resident switch reset streak, 2 subject song song). `run_tests.sh` đã thêm suite này.
    - _Requirements: R4.2, R4.3, R4.4_

- [x] 6. Khâu nối vào `main.cpp` (R5)  ✅
  - [x] 6.1 Cờ CLI `--resident-db`, `--cabin-id` (def 1), `--confirm-streak` (def 5), `--cooldown-ms` (def 3000), `--unknown-after-ms` (def 2000); usage cập nhật, ghi rõ `.fdb`=TEST/DEV vs `--resident-db`=vận hành.
    - _Requirements: R5.1, R5.3_
  - [x] 6.2 Nhánh init khi có `--resident-db`: `ResidentDB.open` → `load_active` → `MatchEngine.build` + map `resident_by_id`/`resident_id_by_name` + config InteractionManager. `use_resident_db` ưu tiên hơn `--face-db`; `.fdb` giữ làm test/dev, in NOTE.
    - _Requirements: R5.1, R5.2, R5.4, R2.1_
  - [x] 6.3 Vòng lặp: match qua `MatchEngine` (resident mode) / `FaceDB` (legacy); build `subjects` (subject_key = track_id nếu tracker, else 0 cho largest face; cached track resolve id qua name→id map); `interaction.update()` → mỗi Outcome `log_event` (matched/unknown) + `touch_resident` khi confirmed; overlay `greeting_name` + `F<floor>`/`F?`.
    - _Requirements: R4.5, R5.2, R1.4, R1.5_
  - [x] 6.4 Cleanup: `resident_db.close()` (flush writer) trước khi `delete recognizer`.
    - _Requirements: R1.4_

- [x] 7. Kiểm thử tích hợp Giai đoạn 1  ✅ (Orange Pi + USB cam, sqlite3 CLI v3.40.1)
  - [x] 7.1 Chạy `main --resident-db` (migrate `faces_all.fdb`, seed `home_floor=7` greeting='anh Sy'): đứng yên 200 frame @14.65fps → **đúng 1** event `matched` (cooldown 3s chống flip-flop). Rời+quay lại 4 lần → **4** event `matched` riêng biệt (cooldown expiry). `match_events`: action='matched', cabin_id=1, resident_id=1, floor_selected=0, latency_ms=4, timestamps (`ts`) giãn đúng. `match_count=5` khớp số event (touch_resident OK). Async writer flush đủ khi close. FPS 14-15 không sụt.
    - _Requirements: R1.4, R4.3, R4.4_
  - [x] 7.2 Chế độ `--face-db` (`.fdb`): match sim=0.53, in NOTE TEST/DEV, KHÔNG ghi SQLite. Vẫn chạy tốt.
    - _Requirements: R5.4_
  - _Ghi chú: action='unknown' chưa test live (không có người lạ đứng đủ lâu) nhưng đã cover bởi 48 unit test InteractionManager._

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
