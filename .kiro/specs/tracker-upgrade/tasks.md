# Implementation Plan — Tracker Upgrade (ByteTrack-lite)

**Spec ID**: `tracker-upgrade`  
**Trạng thái ban đầu**: chưa triển khai  
**Nguyên tắc**: mỗi checkpoint phải build/test được; default giữ `greedy` cho tới khi A/B và live
acceptance đạt.

## Tasks

- [ ] 1. Baseline và deterministic fixtures
  - [ ] 1.1 Ghi baseline tracker hiện tại
    - Chạy/capture các scenario: single motion, crossing, short occlusion, confidence dip,
      `person_every>1`, variable FPS và reconnect gap.
    - Ghi FPS/e2e latency, track IDs, ID switches, fragments và duplicate interaction events.
    - Không commit video/ảnh/PII; chỉ commit detection replay đã loại dữ liệu nhận dạng.
    - _Requirements: R12, R13_
  - [ ] 1.2 Định nghĩa replay fixture schema và evaluator
    - Tạo `tests/fixtures/tracker/` và parser/test helper.
    - Ground-truth ID chỉ tồn tại trong fixture.
    - Evaluator báo IDSW, fragments, created tracks và low-only creations.
    - _Requirements: R12.7, R13.5–R13.9_
  - [ ] 1.3 Thêm fixtures tối thiểu
    - Single constant velocity.
    - Two people crossing.
    - Short occlusion.
    - Low-confidence bridge/false positive.
    - Skipped detector frames và variable timestamps.
    - _Requirements: R12, R13_

- [ ] 2. Refactor tracker API và giữ legacy fallback
  - [ ] 2.1 Tạo shared tracker types
    - Thêm `tracker_types.h`: `TrackerMode`, `TrackState`, `FrameContext`, `TrackIdentity`, `TrackView`,
      `TrackerConfig`.
    - Bảo đảm types không phụ thuộc concrete backend.
    - _Requirements: R1.1–R1.2_
  - [ ] 2.2 Tạo `PersonTracker` interface/factory
    - API predict/update/views/recognition/catalog invalidation.
    - Không trả raw pointer/reference storage nội bộ.
    - _Requirements: R1, R8_
  - [ ] 2.3 Bọc tracker hiện tại thành `GreedyIouTracker`
    - Giữ behavior cũ qua adapter.
    - Chuyển main/overlay/reconcile sang interface và track IDs.
    - Chạy test chứng minh greedy output tương đương fixture baseline.
    - _Requirements: R1.3–R1.6_
  - [ ] 2.4 Thêm `--tracker-mode greedy|byte`
    - Parser/usage/validation/startup log.
    - Default `greedy` trong rollout.
    - _Requirements: R9.5–R9.7, R11.1_
  - [ ] 2.5 Checkpoint API
    - `make -j4`, relevant unit suite, `git diff --check`.
    - Manual review pointer lifetime và shutdown.

- [ ] 3. Hungarian linear assignment
  - [ ] 3.1 Implement solver
    - Thêm `linear_assignment.{h,cpp}`.
    - Rectangular matrix, forbidden cost, empty input, deterministic ties.
    - Không thêm dependency ngoài.
    - _Requirements: R3_
  - [ ] 3.2 Unit tests solver
    - Square, rectangular, empty, forbidden, all-equal, known-optimum matrices.
    - Assert matches và unmatched rows/cols.
    - _Requirements: R12.1_
  - [ ] 3.3 Add Makefile/test runner wiring
    - Build solver vào app và standalone unit test.
    - Check no warning mới.

- [ ] 4. Kalman bounding-box motion
  - [ ] 4.1 Implement `KalmanBoxTracker`
    - State 8D, measurement 4D, dt-aware transition.
    - Initialization covariance, process/measurement noise constants.
    - Sanitize finite/positive boxes.
    - _Requirements: R2.1–R2.5_
  - [ ] 4.2 Handle invalid timestamps/reconnect gaps
    - `dt<=0`, NaN/Inf, `dt>max_predict_gap_ms`.
    - Reset velocity/reinitialize safely; rate-limited numeric log.
    - _Requirements: R2.6, R10.1–R10.2_
  - [ ] 4.3 Kalman tests
    - Stationary, constant velocity, misses, variable dt, large gap, positive size.
    - Deterministic results.
    - _Requirements: R2.7, R12.2_
  - [ ] 4.4 Checkpoint motion
    - Microbenchmark Kalman predict/correct at 1/6/10 tracks.
    - Review float stability on arm64.

- [ ] 5. ByteTrack backend core
  - [ ] 5.1 Implement internal Byte track storage/lifecycle
    - Tentative/Tracked/Lost/Removed.
    - Unique IDs, timestamps, hits, scores, Kalman state.
    - Lifecycle driven by milliseconds.
    - _Requirements: R5_
  - [ ] 5.2 Implement primary high-confidence association
    - Predict boxes → cost matrix → gate → Hungarian → correction.
    - Lost reactivation giữ ID.
    - _Requirements: R3, R4.3, R4.7_
  - [ ] 5.3 Implement secondary low-confidence association
    - Chỉ unmatched recent Tracked candidates.
    - Low detections không tạo tracks.
    - _Requirements: R4.4–R4.6_
  - [ ] 5.4 Implement tentative association và creation
    - Remaining high creates Tentative.
    - Confirm after `confirm_hits`; miss-before-confirm removes.
    - _Requirements: R4.5, R5.2–R5.3_
  - [ ] 5.5 Implement duplicate resolution
    - Deterministic policy based on state/age/hits; không merge chỉ vì một overlap.
    - _Requirements: R4.8_
  - [ ] 5.6 Core tracker tests
    - High/low behavior, lifecycle, reactivation, duplicates, input-order invariance.
    - _Requirements: R12.3_

- [ ] 6. YOLO low-confidence pipeline
  - [ ] 6.1 Refactor YOLO decode threshold
    - Decode một lần từ low threshold; giữ NMS score order.
    - Không thay model layout/anchors/person class behavior.
    - _Requirements: R4.1–R4.2_
  - [ ] 6.2 Add config
    - `--person-thr` = high; thêm `--person-low-thr`.
    - Validate `0 <= low < high <= 1` và invalid args rõ ràng.
    - _Requirements: R9.1–R9.2, R9.6_
  - [ ] 6.3 Add lifecycle timeout config
    - Thêm `--track-lost-ms`; giữ `--track-max-miss` cho greedy.
    - Document migration/precedence.
    - _Requirements: R9.3–R9.4_
  - [ ] 6.4 Decoder regression tests/static validation
    - Verify low candidates retained, NMS high wins duplicate, invalid boxes dropped.
    - Ensure no double decode or extra NPU call.

- [ ] 7. Main loop integration
  - [ ] 7.1 Predict every video frame
    - Tạo `FrameContext` từ monotonic time và actual frame dimensions.
    - Gọi predict kể cả frame không chạy YOLO.
    - Không advance tracker ở DB poll timeout không có frame.
    - _Requirements: R2.2–R2.3, R10.1_
  - [ ] 7.2 Update on detector frames
    - YOLO decode low threshold, tracker tự split tiers/update.
    - TrackView snapshot cho overlay/recognition/interaction.
    - _Requirements: R4, R5_
  - [ ] 7.3 Preserve SCRFD-only branch
    - Không person model → behavior cũ.
    - _Requirements: R1.5_
  - [ ] 7.4 Preserve shutdown/thread model
    - Không background tracker thread.
    - q/ESC, SIGINT và SIGTERM vẫn clean shutdown theo project requirement.
    - _Requirements: R10.3–R10.4_
  - [ ] 7.5 Main integration checkpoint
    - Full build all targets.
    - AppConfig/video wait/tracker tests.
    - Static review NPU init order và exit codes.

- [ ] 8. One-to-one face-to-track association
  - [ ] 8.1 Implement `face_track_assoc.{h,cpp}`
    - Head ROI gate, normalized center/size cost, Hungarian assignment.
    - Return track IDs by face index.
    - _Requirements: R6.1–R6.4_
  - [ ] 8.2 Integrate history tie-break
    - Previous face↔track link chỉ bonus khi geometry vẫn valid.
    - Orphan behavior giữ nguyên.
    - _Requirements: R6.3, R6.5_
  - [ ] 8.3 Face association tests
    - Overlapping person boxes, two faces, one face/two tracks, orphan, reordered input.
    - _Requirements: R12.4_
  - [ ] 8.4 Update overlay/main consumers
    - Không raw Track pointer.
    - Cached recognition label lấy từ TrackView.

- [ ] 9. Resident identity và ghost migration
  - [ ] 9.1 Move authoritative resident identity into Track
    - `resident_id`, display name, similarity, recognition timestamp.
    - Update `record_recognition` API.
    - _Requirements: R7.1–R7.3_
  - [ ] 9.2 Replace name-keyed ghosts
    - Ghost keyed bằng resident ID + TTL; enforce one active owner.
    - Same-name/different-resident không merge.
    - _Requirements: R7.4–R7.5_
  - [ ] 9.3 Implement identity conflict confirmation
    - Pending candidate/streak policy hoặc reuse configured recognition confirmation.
    - Numeric, rate-limited diagnostics.
    - _Requirements: R7.6, R11.2–R11.4_
  - [ ] 9.4 Migrate/remove external `track_resident_id`
    - Update overlay, interaction subject build và reconciliation.
    - End state có một source of truth.
    - _Requirements: R7, R8_
  - [ ] 9.5 Identity tests
    - Same label/different resident, re-entry, active ownership conflict, ghost expiry, privacy.
    - _Requirements: R12.5–R12.6_

- [ ] 10. Resident hot reload reconciliation
  - [ ] 10.1 Update tracker invalidation API
    - Added invalidates unknown; changed/removed clears matching resident cache.
    - Geometry/motion/lifecycle giữ nguyên.
    - _Requirements: R8.1–R8.4_
  - [ ] 10.2 Update `resident_reconcile`
    - Reconcile affected interaction subjects ở same frame boundary.
    - Preserve unaffected cooldown/history.
    - _Requirements: R8.5–R8.7_
  - [ ] 10.3 Regression tests
    - Existing resident catalog/reconcile/interaction tests pass.
    - Add byte-backend variants and no duplicate event test.

- [ ] 11. Observability và benchmark
  - [ ] 11.1 Startup config log
    - Mode, high/low, gates, lost timeout, confirm hits; no PII.
    - _Requirements: R11.1_
  - [ ] 11.2 Runtime diagnostics
    - Rate-limit health/transitions; per-match only DEBUG/TRACE.
    - _Requirements: R11.2–R11.4_
  - [ ] 11.3 Benchmark tracker stages
    - Predict/associate p50/p95, active/lost counts.
    - Không phá benchmark output contract nếu tooling đang parse format.
    - _Requirements: R11.5, R13.3–R13.4_
  - [ ] 11.4 Privacy scan
    - Không name/greeting/embedding/RTSP URL trong new logs.

- [ ] 12. Automated verification
  - [ ] 12.1 Run formatting/static checks
    - `git diff --check`, shell syntax, compiler warnings.
  - [ ] 12.2 Run focused tests
    - Linear assignment, Kalman, tracker, face association, replay, resident reconcile.
  - [ ] 12.3 Run full suite
    - Record unrelated blockers separately; không bỏ qua tracker failure.
  - [ ] 12.4 Build all targets
    - `make -j4`; `make -q` clean afterward.
    - _Requirements: R13.1–R13.2_
  - [ ] 12.5 A/B replay report
    - Greedy và byte từ cùng fixtures; IDSW/fragments/low-only creations/latency.
    - _Requirements: R13.5–R13.9_

- [ ] 13. Orange Pi live validation
  - [ ] 13.1 Recorded/live performance
    - Cùng source/config, đo FPS/e2e/tracker p95.
    - Gate tracker <1 ms và FPS regression ≤2%.
    - _Requirements: R13.3–R13.4_
  - [ ] 13.2 Cabin scenarios
    - Single entry/exit, crossing, 0.5–2 s occlusion, turn away/back, multi-entry.
    - Kiểm tra stable ID, face link, recognition cache và duplicate greeting.
  - [ ] 13.3 Detector scheduling
    - `person_every=1/2/3`; predicted overlay và timeout theo ms.
  - [ ] 13.4 RTSP reconnect
    - Gap lớn reset velocity; app reconnect và tiếp tục cleanly.
  - [ ] 13.5 Hot reload và shutdown
    - Add/change/remove resident khi tracker active.
    - q/ESC, SIGINT, SIGTERM clean shutdown.

- [ ] 14. Rollout
  - [ ] 14.1 Canary với `--tracker-mode byte`
    - Một cabin, aggregate metrics không PII, rollback command được xác nhận.
  - [ ] 14.2 Acceptance review
    - IDSW/fragments thấp hơn baseline; mục tiêu ≥50%.
    - Không duplicate greeting/event; performance gates đạt.
  - [ ] 14.3 Đổi default sang byte
    - Update AppConfig/env/README và startup docs.
    - Giữ greedy fallback một release cycle.
  - [ ] 14.4 Optional legacy removal follow-up
    - Chỉ tạo task sau soak/sign-off; không xóa fallback trong cùng rollout commit.

- [ ] 15. Documentation và sign-off
  - [ ] 15.1 Update README CLI/tracker behavior
  - [ ] 15.2 Update `docs/DEVELOPMENT_PLAN.md` status/evidence
  - [ ] 15.3 Record benchmark hardware, inputs, config và limitations
  - [ ] 15.4 Final privacy/security review
  - [ ] 15.5 Mark spec complete khi mọi acceptance gate đạt

## Checkpoints

1. **API checkpoint** — sau Task 2: behavior greedy không đổi, không raw pointer.
2. **Algorithm checkpoint** — sau Task 5: ByteTrack core pass deterministic tests.
3. **Pipeline checkpoint** — sau Task 8: main + face association build/run.
4. **Identity checkpoint** — sau Task 10: hot reload/interaction/privacy pass.
5. **Release checkpoint** — sau Task 13: A733 live evidence đạt trước đổi default.

## Suggested commit sequence

1. `test(tracker): add deterministic replay fixtures`
2. `refactor(tracker): introduce backend interface and legacy adapter`
3. `feat(tracker): add Hungarian linear assignment`
4. `feat(tracker): add Kalman box prediction`
5. `feat(tracker): add two-stage ByteTrack association`
6. `feat(config): add ByteTrack thresholds and timeout`
7. `feat(tracker): predict across skipped detector frames`
8. `feat(tracker): add one-to-one face association`
9. `fix(tracker): key recognition and ghosts by resident id`
10. `test(tracker): cover reload crossing occlusion and privacy`
11. `docs(tracker): record A-B and live validation evidence`
12. `feat(tracker): enable ByteTrack backend by default`

## Dependency graph

```json
{
  "waves": [
    {"id": 0, "tasks": ["1"]},
    {"id": 1, "tasks": ["2", "3"]},
    {"id": 2, "tasks": ["4"]},
    {"id": 3, "tasks": ["5", "6"]},
    {"id": 4, "tasks": ["7"]},
    {"id": 5, "tasks": ["8", "9"]},
    {"id": 6, "tasks": ["10", "11"]},
    {"id": 7, "tasks": ["12"]},
    {"id": 8, "tasks": ["13"]},
    {"id": 9, "tasks": ["14", "15"]}
  ]
}
```

