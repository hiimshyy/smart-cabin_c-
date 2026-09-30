# Tasks — Resident Hot Reload

**Spec ID**: `resident-hot-reload`

---

## 📌 TRẠNG THÁI

**Chưa bắt đầu code.** Requirements/design đã chốt hướng polling SQLite 1000 ms, không đổi schema,
reader connection riêng, candidate-build-then-swap và cache invalidation có chọn lọc.

**Quyết định đã chốt:**
- Chỉ bật trong `--resident-db`; `.fdb` giữ nguyên.
- `--resident-reload-ms` mặc định 1000, `0` tắt/rollback.
- `add_person` write batch atomic trong một transaction; JSON ok:true chỉ sau COMMIT.
- `PRAGMA data_version` là bộ lọc; content fingerprint mới quyết định reload.
- Snapshot residents+embeddings trong cùng read transaction, query có ORDER BY.
- Reader read-only/no-migration, timeout 25–100ms; reconnect force full snapshot.
- Poll deadline độc lập frame arrival (timed wait khi camera stall).
- Reload fail giữ matcher generation cũ và retry; không partial swap.
- Invalidate affected tracker/session, purge ghost cache nhưng giữ person track/cooldown.
- Strict blob validation: declared dim/exact bytes/finite/nonzero/L2 tolerance.
- Không schema migration, không sửa Node/web để trigger reload.
- Privacy R6: không log names/greeting/apartment/image path/vector.

---

- [x] 0. Atomic enroll write (`add_person` correctness prerequisite)
  - [x] 0.1 Sau khi extract embeddings xong, bọc upsert resident + optional delete + toàn bộ inserts
        trong `BEGIN IMMEDIATE`/COMMIT; mọi fail ROLLBACK bằng guard an toàn.
    - _Requirements: R3.1, R8.4_
  - [x] 0.2 Chỉ in JSON `ok:true` sau COMMIT; partial write/commit fail trả JSON db error và giữ catalog
        trước nguyên vẹn.
    - _Requirements: R3.1, acceptance 10_
  - [x] 0.3 Tách batch SQLite thuần khỏi NPU/image extraction thành helper testable (hoặc fault-injection
        seam deterministic). Test lỗi giữa batch replace nhiều ảnh: old embeddings/metadata không mất;
        success commit một generation đầy đủ và JSON/commit contract đúng.
    - _Requirements: R3.1, acceptance 10_

- [x] 1. ResidentDB reader primitives
  - [x] 1.1 Thêm `ResidentDB::open_readonly(path, busy_timeout_ms=50)` dùng READONLY/query_only,
        không WAL/migration/writer; và `data_version()` đọc `PRAGMA data_version`.
    - _Requirements: R2.1–R2.4, R3.4, R7.2_
  - [x] 1.2 Thêm `load_active_snapshot()`: `BEGIN` read transaction → SELECT active residents
        `ORDER BY id` → SELECT `e.id,resident_id,source,dim,vector,length(vector)`
        `ORDER BY resident_id,id` → kiểm terminal SQLITE_DONE → COMMIT; mọi lỗi ROLLBACK và không trả
        dữ liệu nửa chừng. Mở rộng EmbeddingRow với metadata validation cần thiết.
    - _Requirements: R3.2–R3.5, R4.1–R4.3_
  - [x] 1.3 Giữ `load_active()` tương thích (delegate snapshot hoặc giữ wrapper); xác nhận không gọi
        `BEGIN IMMEDIATE` trên reader.
    - _Requirements: R3.4, R8.2_
  - [x] 1.4 Thêm test-only phase hook/barrier sau SELECT residents; test deterministic external commit
        giữa hai SELECT. Test busy trả trong timeout ngắn, reconnect force full snapshot và connection
        dùng tiếp được.
    - _Requirements: R2, R3, R7.2_

- [x] 2. Module `resident_catalog` — fingerprint/candidate/diff
  - [x] 2.1 Tạo `src/resident_catalog.{h,cpp}` với `CatalogFingerprint`, `ResidentDigest`,
        `CatalogDiff`, candidate/generation structs; hash deterministic toàn bộ runtime metadata +
        embedding vector bytes, không hash audit fields.
    - _Requirements: R2.5–R2.7, R6.4_
  - [x] 2.2 Validate tất cả embedding: owner active tồn tại, blob non-NULL, declared dim đúng,
        exact byte length, mọi float finite, norm > epsilon và trong L2 tolerance; catalog rỗng hợp
        lệ; lỗi từ chối toàn candidate.
    - _Requirements: R4.1–R4.4, R4.9_
  - [x] 2.3 Build temporary `MatchEngine` + `resident_by_id`; tính diff added/changed/removed theo
        per-resident digest; expose output move-only để main swap atomically.
    - _Requirements: R4.3–R4.6_
  - [x] 2.4 Unit/property tests: fingerprint không phụ thuộc row order; audit/touch không đổi;
        metadata/vector/add/replace/delete/deactivate đổi đúng; corrupt blob/dim giữ generation cũ.
    - _Requirements: R2, R4_

- [ ] 3. `ResidentCatalogReloader` poll state machine
  - [x] 3.1 Implement interval scheduling bằng monotonic time; fast path data_version unchanged;
        pending failure retry; reader open/reopen backoff 1–10s, mỗi reconnect reset accepted version
        và force full snapshot.
    - _Requirements: R1.3–R1.4, R2.2–R2.5, R3.6–R3.7_
  - [x] 3.2 Khi data_version đổi: snapshot → fingerprint; fingerprint bằng thì accepted version mới
        nhưng không build/swap; fingerprint khác thì tạo candidate.
    - _Requirements: R2.4–R2.7, R7.1–R7.3_
  - [ ] 3.3 Counters/log state: polls, version changes, content changes, success/failure; rate-limit lỗi;
        không PII.
    - _Requirements: R6_
  - [x] 3.4 Integration test hai SQLite connections: external enroll-equivalent commit reload đúng một
        lần; nhiều match_events/touch commits không tăng generation; failure retry thành công.
    - _Requirements: R2, R3, R7_

- [x] 4. Cache invalidation có chọn lọc
  - [x] 4.1 Thêm Tracker API invalidate unknown/unrecognized tracks và tracks map tới changed/removed
        resident IDs; clear name/sim/last_recog nhưng giữ bbox, track ID/state/hit/miss. Purge toàn bộ
        ghost cache khi content catalog đổi (Ghost hiện không có resident_id).
    - _Requirements: R5.1–R5.5_
  - [x] 4.2 Thêm `InteractionManager::drop_sessions(subject_keys)` chỉ erase affected sessions, giữ
        `last_confirmed_ms_` cooldown history.
    - _Requirements: R5.6_
  - [x] 4.3 Helper reconcile `track_resident_id`: xóa mapping affected; added resident invalidates unknown
        tracks; changed/removed invalidates mapped tracks. SCRFD-only không drop key 0 khi chỉ add
        resident khác; chỉ reconcile nếu SessionView resident hiện tại thuộc changed/removed.
    - _Requirements: R5.1–R5.8_
  - [x] 4.4 Thay `Tracker::try_inherit_identity` printf chứa name bằng structured logger không PII.
        Unit test geometry/ID preserved + next `needs_recog=true`; affected-only session reset và
        cooldown vẫn chặn duplicate; ghost bị purge; duplicate-name/privacy regression. Thêm case:
        confirm A, để cooldown hết, add/change B không liên quan, tiếp tục match A và assert không có
        confirmed/floor-action mới.
    - _Requirements: R5, R6.4_

- [ ] 5. CLI/config wiring
  - [ ] 5.1 Thêm `AppConfig::resident_reload_ms=1000`, parser/help `--resident-reload-ms N`; validate N≥0.
    - _Requirements: R1.1–R1.5_
  - [ ] 5.2 `--resident-reload-ms 0` disable hoàn toàn reader/poll và giữ behavior load-once hiện tại.
    - _Requirements: R1.2, R8.3_
  - [ ] 5.3 Parser unit test/default/help behavior; cập nhật `env.sh face_help` nếu có liệt kê option.
    - _Requirements: R1, R8_

- [ ] 6. Main-loop integration
  - [ ] 6.1 Trong resident DB mode, mở dedicated catalog reader bằng `open_readonly()` timeout ngắn;
        initial catalog dùng cùng snapshot/validation path để tránh logic startup và reload lệch nhau.
    - _Requirements: R2.1, R3, R8.2_
  - [ ] 6.2 Đổi capture wait sang `wait_until(next_reload_deadline)` khi reload enabled: deadline tới
        thì poll dù không có frame; không frame mới thì quay lại wait, không chạy NPU/busy-loop. Khi
        có candidate, build ngoài DB transaction, swap matcher + map cùng block rồi reconcile cache.
    - _Requirements: R1.3–R1.4, R4.5–R4.8, R5.8, R7.3–R7.5_
  - [ ] 6.3 Failure path giữ active objects; catalog rỗng hợp lệ; reader close trong shutdown trước
        teardown cuối.
    - _Requirements: R3.4–R3.6, R4.7, R7.5_
  - [ ] 6.4 Log startup/reload đúng privacy: interval/generation/count/latency/affected tracks, không tên.
    - _Requirements: R6_

- [ ] 7. Build và validation không NPU
  - [ ] 7.1 Thêm source/test vào Makefile + `tests/run_tests.sh`; build `make -j4` không warning mới.
    - _Requirements: R8_
  - [ ] 7.2 Chạy full unit suite; test catalog quy mô giả lập (0, 1, 1000 residents; multi-embedding).
    - _Requirements: R4, R7.1_
  - [ ] 7.3 Regression test: `.fdb` mode không mở poll reader; reload disabled tương đương load-once;
        capture không publish frame vẫn poll đúng cadence; reader busy không block ~5s.
    - _Requirements: R1.1–R1.4, R7.2, R8.2–R8.4_

- [ ] 8. Smoke test Orange Pi end-to-end (một lần có kiểm soát)
  - [ ] 8.1 Đảm bảo chỉ một `face_recog_app`; dùng `timeout -k` nếu chạy benchmark tự động để không lặp
        lỗi smoke process sống sót.
    - _Requirements: R7.5_
  - [ ] 8.2 Start app với resident A, giữ PID; concurrent enroll B (nhiều ảnh + `--replace`) bằng
        `add_person`; xác nhận add commit atomic, PID app không đổi, log generation tăng đúng một lần
        trong ~1 giây và không có generation zero/partial embeddings.
    - _Requirements: R3.1, acceptance 1–4, 10_
  - [ ] 8.3 Đặt B trước camera trước/sau enroll; xác nhận unknown track recognition lại ngay và match B
        không cần restart; A vẫn match; không duplicate confirm chỉ do reload.
    - _Requirements: R5, acceptance 4–5_
  - [ ] 8.4 Test audit-only không reload; inject/recover bad candidate hoặc busy; app giữ generation cũ.
    - _Requirements: acceptance 6–7_
  - [ ] 8.5 Benchmark catalog không đổi trước/sau; ghi nhận poll overhead và NPU contention trong thời
        gian `add_person` riêng biệt.
    - _Requirements: R7, acceptance 8_

- [ ] 9. Documentation và rollout
  - [ ] 9.1 README: enroll khi app chạy → NPU có thể khựng ngắn, catalog auto reload; cách disable
        `--resident-reload-ms 0`; lần deploy binary mới vẫn cần restart một lần.
  - [ ] 9.2 DEVELOPMENT_PLAN: Phương án A từ restart-after-enroll → SQLite hot reload; UDS đợt b vẫn là
        đích dài hạn.
  - [ ] 9.3 Ghi runbook kiểm tra generation/counters, lỗi reload và rollback; nhắc privacy/no names.
    - _Requirements: R6, R8_

---

## Thứ tự milestone/commit đề xuất

1. **Atomic add_person batch** (Task 0) — correctness prerequisite trước khi poll.
2. **Read-only DB snapshot primitives** (Task 1) — deterministic barrier tests, không NPU.
3. **Catalog fingerprint/reloader** (Task 2–3) — test hai connection + false-positive audit/reconnect.
4. **Tracker/interaction/ghost reconciliation** (Task 4) — test độc lập + privacy.
5. **CLI + timed main-loop wiring** (Task 5–6) — build đầy đủ, no-frame poll test.
6. **Validation + smoke** (Task 7–8) — chỉ một lần NPU có hard timeout.
7. **Docs** (Task 9).

Mỗi milestone commit cục bộ riêng; user tự `git push`. Không commit DB, backup hoặc ảnh test.
