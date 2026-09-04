# Tasks — Lớp SQLite + Multi-embedding + State Machine tương tác

**Spec ID**: `resident-db-layer`

- [ ] 1. Cập nhật build cho SQLite
  - Thêm `-lsqlite3` vào `LIBS` trong Makefile; ghi chú cài `libsqlite3-dev`.
  - Xác nhận build hiện tại vẫn pass sau khi thêm lib.
  - _Requirements: R5, NFR build_

- [ ] 2. `MatchEngine` (multi-embedding) + unit test
  - [ ] 2.1 Tạo `src/match_engine.h/.cpp`: `MatchResult`, class `MatchEngine` (flat array, owner map), `build()`/`match()` cosine max/`vector_count()`. Tái dùng cosine của `face_db.cpp`.
    - _Requirements: R2.1, R2.2, R2.3, R2.5_
  - [ ] 2.2 Unit test (máy dev): vector giả → match đúng resident max cosine; dưới ngưỡng → unknown.
    - _Requirements: R2.2, R2.3_

- [ ] 3. `ResidentDB` (wrapper SQLite) + unit test
  - [ ] 3.1 `src/resident_db.h/.cpp`: `open()` (WAL, foreign_keys, apply `db/schema.sql` nếu trống).
    - _Requirements: R1.1, R1.2, R1.6_
  - [ ] 3.2 `load_active()` đọc residents active=1 + embeddings; struct `Resident`/`EmbeddingRow`.
    - _Requirements: R1.3, R2.1_
  - [ ] 3.3 Async event writer: `log_event()` push queue + thread nền batch; `touch_resident()`.
    - _Requirements: R1.4, R1.5, NFR non-blocking_
  - [ ] 3.4 `close()` flush + join writer thread.
    - _Requirements: R1.4_
  - [ ] 3.5 Unit test: open tạo schema; insert+load round-trip; log_event ghi được; touch tăng count.
    - _Requirements: R1.2, R1.3, R1.4, R1.5_

- [ ] 4. Tool `migrate_fdb` (R3)
  - [ ] 4.1 `src/migrate_fdb.cpp`: đọc `.fdb` → insert residents (`home_floor = 0` sentinel, req §6) + embeddings(source='id_photo'); trùng name skip mặc định, `--overwrite` thay thế; log tổng số resident chưa có tầng.
    - _Requirements: R3.1, R3.2, R3.3, §6_
  - [ ] 4.2 Target `migrate_fdb` vào Makefile (không cần SDK NPU); in tổng kết import/skip.
    - _Requirements: R3.4, NFR build_
  - [ ] 4.3 Test: fdb mẫu → SQLite đúng số residents/embeddings; chạy lại skip/overwrite đúng.
    - _Requirements: R3.1-R3.4_

- [ ] 5. `InteractionManager` (state machine) + unit test
  - [ ] 5.1 `src/interaction.h/.cpp`: `SessionState`, `Session`, `InteractionConfig`, `update()` trả Outcome; DETECTING→MATCHED→CONFIRMED theo streak; cooldown per resident; unknown timeout.
    - _Requirements: R4.1, R4.2, R4.3, R4.4_
  - [ ] 5.2 Unit test: chuỗi frame giả → CONFIRMED sau đúng streak; cooldown chặn lần 2; unknown sau timeout.
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

- [ ] 8. Bổ sung API ghi vào `ResidentDB`
  - `upsert_resident(name, home_floor=0, ...) -> id`, `add_embedding(resident_id, source, vector)`, `delete_embeddings(resident_id)`.
  - Unit test round-trip cho 3 hàm.
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
