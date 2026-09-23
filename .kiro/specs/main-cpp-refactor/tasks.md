# Implementation Plan: main.cpp Refactor

## Overview

Pure refactor of `src/main.cpp` (~980 lines) into four cohesive modules plus a
shared data-only header, leaving `main.cpp` as model init + pipeline
orchestration + shutdown. The north star is **byte-for-byte behavioral
equivalence** with the Baseline_App: same CLI surface, log lines, benchmark
table, recognition/matching/interaction outcomes, exit codes, and threading.

Every extraction MOVES code verbatim — identical function bodies, signatures,
log strings, `printf` format specifiers, and usage text. No rewrites, no new
classes, no ownership changes. Extraction is sequenced lowest-risk-first:
Benchmark → Config → Video_IO → Overlay → main-trim → Makefile.

Property-based tests (RapidCheck, min 100 iterations) run on the Windows dev
host and validate the five design properties against a frozen Baseline model.
`make` build and full runtime differential verification require the Linux target
toolchain and are marked **blocked-pending-Linux** (Req 6.2).

## Tasks

- [ ] 1. Extract Benchmark_Module (lowest risk — pure accounting + printf)
  - [ ] 1.1 Create `src/benchmark.h` and `src/benchmark.cpp`
    - Move `StageStat` struct verbatim (fields `sum/mn/mx/n`, methods `add()`, `avg()`) into `benchmark.h`
    - Declare `print_benchmark_summary(run_secs, frame_id, use_tracker, person_every, cap, pre, yolo, scrfd, recog, draw, e2e)` in `benchmark.h`
    - Move the entire `===== BENCHMARK SUMMARY =====` printf block verbatim into `print_benchmark_summary` in `benchmark.cpp`, preserving every row label, column ordering, and numeric format specifier, including the conditional `yolo (%dx)` row gated on `use_tracker && s_yolo.n > 0`
    - _Requirements: 3.3, 4.4, 4.5_
  - [ ]* 1.2 Write property test for benchmark-table byte-equality
    - **Property 4: Benchmark-table byte-equality**
    - **Validates: Requirements 3.3**
    - RapidCheck, min 100 iterations, tagged "Feature: main-cpp-refactor, Property 4: benchmark-table byte-equality"
    - Generator: random stat aggregates + `use_tracker`/`person_every`; capture stdout from `print_benchmark_summary` and compare to a frozen Baseline `printf` model checked into the test

- [ ] 2. Extract Config_Module (CLI parsing + usage text)
  - [ ] 2.1 Create `src/app_config.h` with `AppConfig` and interface
    - Move `AppConfig` struct with every parsed field and its exact Baseline default (paths, `cam_id`, `max_frames`, `recog_dim`, backoff/latency, tracker/recog params, thresholds, `recog_rgb`, `fullscreen`)
    - Declare `print_usage(const char* prog)` and `ParseResult parse_args(int argc, char** argv, AppConfig& cfg)`
    - _Requirements: 2.2, 4.3_
  - [ ] 2.2 Implement `parse_args` and `print_usage` in `src/app_config.cpp`
    - Move the Baseline argv loop verbatim: positional handling (at most 2 — det model path then cam id, extra positionals ignored), the `sv()` value-flag helper requiring `i+1 < argc`, `atoi`/`atof` coercion, unknown/malformed-flag silent ignore, `-h/--help` → print usage + report help request
    - Move `print_usage` format string byte-for-byte, emitting to stderr
    - _Requirements: 2.1, 2.2, 2.4, 2.5, 2.7, 4.3_
  - [ ]* 2.3 Write property test for CLI parse differential equivalence
    - **Property 1: CLI parse differential equivalence**
    - **Validates: Requirements 2.1, 2.2, 2.4, 2.7**
    - RapidCheck, min 100 iterations, tagged "Feature: main-cpp-refactor, Property 1: CLI parse differential equivalence"
    - Generator: random `argv` mixing valid flags, omitted flags, extra positionals beyond two, unknown flags, dangling value-flags, non-numeric values for numeric flags; compare every `AppConfig` field to a frozen Baseline argv-loop model
  - [ ]* 2.4 Write property test for usage-text byte-equality
    - **Property 3: Usage-text byte-equality**
    - **Validates: Requirements 2.5**
    - RapidCheck, min 100 iterations, tagged "Feature: main-cpp-refactor, Property 3: usage-text byte-equality"
    - Generator: random `prog` strings; capture stderr from `print_usage(prog)` and compare character-for-character to the frozen Baseline usage snapshot with `prog` substituted

- [ ] 3. Checkpoint - review Benchmark + Config extraction
  - Ensure all property tests pass, ask the user if questions arise.

- [ ] 4. Extract Video_IO_Module (capture + display threads + gst helpers)
  - [ ] 4.1 Create `src/video_io.h` with slots and function declarations
    - Move `FrameSlot`, `CamConfig`, `DisplaySlot` structs verbatim (synchronization fields `std::mutex`/`std::condition_variable`/`seq`/`stop` unchanged to preserve the producer/consumer handshake)
    - Declare `open_capture`, `capture_worker`, `display_worker` (keeping the `std::atomic<bool>* stop_flag` argument so `g_stop` stays owned by main), `build_gst_pipeline`, `is_stream_source`
    - _Requirements: 1.4, 4.1_
  - [ ] 4.2 Implement the six functions verbatim in `src/video_io.cpp`
    - Move `open_capture`, `capture_worker` (including reconnect/backoff loop and `"cam"` log warnings), `display_worker` (namedWindow/fullscreen, imshow, q/ESC handling, destroyWindow), `build_gst_pipeline`, `is_stream_source` with identical bodies
    - Preserve every `LOG_INFO`/`LOG_WARN` `"cam"` string verbatim
    - _Requirements: 1.4, 3.1, 4.1_

- [ ] 5. Extract face_label.h and Overlay_Module
  - [ ] 5.1 Create shared data-only `src/face_label.h`
    - Define `FaceLabel` (fields `name`, `sim`, `stat`, `track_id`, `resident_id`, `area`) as the single documented intentional structural addition, included by both `main.cpp` and `overlay.cpp`
    - _Requirements: 4.5, 6.4_
  - [ ] 5.2 Create `src/overlay.h` with `UiScale` and draw function declarations
    - Move `UiScale` struct + `static UiScale compute(int frame_h, float override_val)` verbatim
    - Declare `draw_tracker_overlay`, `draw_scrfd_overlay`, `draw_hud` with signatures taking loop data by reference/value (no I/O, no state)
    - _Requirements: 4.2_
  - [ ] 5.3 Implement `UiScale::compute` and draw functions verbatim in `src/overlay.cpp`
    - Move `UiScale::compute` body verbatim (all thicknesses, font scales, HUD geometry, landmark radius)
    - Move the tracker-mode overlay branch (person track rectangles + labels, then face bboxes + landmarks), the SCRFD-only overlay branch (face bbox + label + landmarks), and the HUD bar (`addWeighted` darkened strip + `putText`) verbatim, keeping all colors, thickness, font scales, and label `snprintf` formats byte-identical
    - _Requirements: 1.1, 3.1, 4.2_
  - [ ]* 5.4 Write property test for UiScale::compute equivalence
    - **Property 5: UiScale::compute equivalence**
    - **Validates: Requirements 1.1**
    - RapidCheck, min 100 iterations, tagged "Feature: main-cpp-refactor, Property 5: UiScale::compute equivalence"
    - Generator: random `frame_h` (incl. 0, 480, 720, 1080, large) and `override_val` (incl. 0, negative, fractional); compare every field to the frozen Baseline computation

- [ ] 6. Checkpoint - review Video_IO + Overlay extraction
  - Ensure all property tests pass, ask the user if questions arise.

- [ ] 7. Trim main.cpp to model init + pipeline orchestration + shutdown
  - [ ] 7.1 Wire main.cpp to the extracted modules and remove moved definitions
    - Include `app_config.h`, `video_io.h`, `overlay.h`, `benchmark.h`, `face_label.h`; delete the moved struct/function definitions from `main.cpp`
    - Replace inline CLI parsing with `parse_args` + `-h/--help` → `return 0`; compute derived flags (`use_tracker`, `recog_enabled`, `is_stream`, resident-db-over-face-db precedence) in `main` from `AppConfig` exactly as today
    - Keep `g_stop` (atomic) + `on_signal` + `std::signal` registration in `main`; spawn exactly one capture and one display thread via `capture_worker`/`display_worker`
    - Preserve NPU model-init order (recognizer → resident/face DB → SCRFD → YOLO) and exit codes 1 (camera open), 2 (recognizer or resident-DB init), 3 (SCRFD init), 4 (YOLO create/init) at each `return` site
    - Keep `FaceLabel` face→track association, recognition scheduling, DEBUG per-frame detail log, interaction state machine + `"event"` match-event logging, `--frames` break, and the Req 1.5 shutdown sequence order
    - Call `UiScale::compute`/`draw_*` (Overlay), `StageStat`/`print_benchmark_summary` (Benchmark), and `resolve_log_config` (pre-existing logger) in place
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 3.4, 3.5, 3.6, 4.5, 4.6_
  - [ ]* 7.2 Write property test for derived-flag equivalence
    - **Property 2: Derived-flag equivalence**
    - **Validates: Requirements 2.6**
    - RapidCheck, min 100 iterations, tagged "Feature: main-cpp-refactor, Property 2: derived-flag equivalence"
    - Generator: random `AppConfig` path/flag combinations; compare `use_tracker`, `recog_enabled`, `is_stream`, and resident-db precedence to the frozen Baseline derivation

- [ ] 8. Update Makefile for the new app-only modules
  - [ ] 8.1 Add `APP_ONLY_SRCS` and append it to `APP_SRCS_CPP`
    - After the `DB_SRCS` block, add `APP_ONLY_SRCS := app_config.cpp video_io.cpp overlay.cpp benchmark.cpp` (with `$(SRC_DIR)/` prefixes)
    - Append `$(APP_ONLY_SRCS)` to `APP_SRCS_CPP` only; do NOT add to `COMMON_SRCS` (these are used solely by `face_recog_app`)
    - Leave `INCLUDES`, `LIBS`, `CXXFLAGS`, pattern rules, and all other target source lists unchanged
    - _Requirements: 4.7, 5.1, 5.2, 5.3_

- [ ] 9. Static-review checklist on the Windows dev host
  - Verify each of the 9 items from the design's Verification Environment Constraint section: (1) each moved function body diff-identical to Baseline (control flow, signatures); (2) every `LOG_*` string preserved verbatim, no name/greeting/apartment logged; (3) every benchmark `printf` line verbatim; (4) usage text verbatim; (5) model-init order + exit codes 1/2/3/4 unchanged; (6) shutdown sequence order + `--frames` break unchanged; (7) exactly one capture + one display thread spawned; (8) Makefile only `APP_SRCS_CPP` extended with `APP_ONLY_SRCS`, `LIBS` unchanged; (9) the `FaceLabel` → `face_label.h` change documented
  - Resolve or explicitly record as intentional any detected difference
  - _Requirements: 3.4, 3.5, 3.6, 6.3, 6.4_

- [ ] 10. Linux build verification (blocked-pending-Linux — Req 6.2)
  - [ ]* 10.1 Run `make all` on the Target_Toolchain
    - Confirm new `.cpp` files compile through the `src/%.cpp -> build/%.o` rule (5.1), `face_recog_app` links with zero unresolved symbols (5.2, 4.6), same `LIBS` set (5.3), and no new warnings under `-Wall` vs Baseline (5.4)
    - Deferred: developer host is Windows and cannot run the Linux/make toolchain
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 6.2, 6.5_

- [ ] 11. Linux runtime differential verification (blocked-pending-Linux — Req 6.2)
  - [ ]* 11.1 Full Pipeline_Behavior differential run
    - Run Baseline_App and Refactored_App over an identical recorded frame sequence with fixed `--frames`, once per mode (SCRFD-only, YOLO+tracker); diff normalized log output and exit code
    - Deferred pending Linux environment
    - _Requirements: 1.1, 3.1, 3.2, 6.1, 6.2, 6.5_
  - [ ]* 11.2 Log-line diff and q/ESC shutdown check
    - Diff logs with timestamp/run-id fields normalized (3.1, 3.2); one GUI run confirming q/ESC stops the pipeline with the Req 1.5 shutdown sequence and exit code 0 (1.6)
    - Deferred pending Linux environment
    - _Requirements: 1.5, 1.6, 3.1, 3.2, 6.2, 6.5_

## Notes

- Tasks marked with `*` are optional and can be skipped for a faster path.
- Property tests (1.2, 2.3, 2.4, 5.4, 7.2) run on the Windows dev host and need no NPU/OpenCV runtime beyond header types.
- Tasks 10 and 11 are Linux-only and marked blocked-pending-Linux per Req 6.2 — they are the sign-off gate (Req 6.5) but cannot run on the Windows dev host.
- Every extraction moves code verbatim; the sole intentional structural change is promoting `FaceLabel` to `face_label.h` (Req 6.4).
- Checkpoints (tasks 3, 6) ensure incremental validation between extractions.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "2.1", "4.1", "5.1"] },
    { "id": 1, "tasks": ["1.2", "2.2", "4.2", "5.2"] },
    { "id": 2, "tasks": ["2.3", "2.4", "5.3"] },
    { "id": 3, "tasks": ["5.4", "7.1"] },
    { "id": 4, "tasks": ["7.2", "8.1"] },
    { "id": 5, "tasks": ["10.1", "11.1", "11.2"] }
  ]
}
```
