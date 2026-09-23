# Requirements Document

## Introduction

The real-time face recognition application for the Orange Pi A733 NPU currently
concentrates most of its logic in a single translation unit, `src/main.cpp`
(~980 lines). That file mixes six distinct responsibilities: CLI argument
parsing, threaded video capture with reconnect, threaded display, UI scaling and
overlay drawing, the main recognition pipeline loop, and benchmark accounting.
The size and coupling make the file hard to read, review, and test.

This feature is a **pure refactor**. The goal is to extract cohesive
responsibilities from `main.cpp` into smaller, self-contained modules while
producing an executable whose observable behavior is byte-for-byte identical to
the pre-refactor build: same CLI surface, same log lines and format, same
benchmark table, same recognition/matching/interaction outcomes, and same
threading behavior.

No new features are added, no flags change, no log strings change, and no
runtime dependency is introduced or removed. The refactor targets four candidate
modules — video I/O, overlay/UI, application configuration/CLI, and benchmark —
with `main.cpp` reduced to model initialization and pipeline orchestration. The
final module boundaries are settled in the design phase.

A hard environmental constraint applies: the developer's machine is Windows and
cannot run the Linux/`make` toolchain or the AI SDK. Compilation and `make`
verification must occur on the target board or a Linux environment. The
acceptance goal for this spec is therefore "code compiles cleanly on the target
toolchain and produces behavior byte-for-byte identical to the pre-refactor
build," with build verification explicitly deferred to a Linux environment.

## Glossary

- **Refactored_App**: The `face_recog_app` executable produced after the
  refactor, built from the split modules.
- **Baseline_App**: The `face_recog_app` executable built from `src/main.cpp`
  before the refactor, used as the behavioral reference.
- **Main_Module**: The post-refactor `src/main.cpp`, reduced to model
  initialization and pipeline orchestration.
- **Video_IO_Module**: The extracted module responsible for threaded video
  capture (`FrameSlot`, `CamConfig`, `open_capture`, `capture_worker`,
  `build_gst_pipeline`, `is_stream_source`) and threaded display (`DisplaySlot`,
  `display_worker`).
- **Overlay_Module**: The extracted module responsible for UI scaling
  (`UiScale`) and overlay/HUD drawing for both tracker and SCRFD-only branches.
- **Config_Module**: The extracted module responsible for CLI argument parsing,
  positional argument handling, and `print_usage`.
- **Benchmark_Module**: The extracted module responsible for stage timing
  accounting (`StageStat`) and the end-of-run benchmark summary table.
- **CLI_Surface**: The complete set of positional arguments, option flags, flag
  aliases, default values, and usage text accepted by the executable.
- **Log_Output**: The set of tag/level/format strings emitted through the
  application logger (`LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_DEBUG`) and the
  `printf`-based benchmark table.
- **Pipeline_Behavior**: The per-frame processing sequence and its observable
  results — preprocess, YOLO person detection, tracker update, SCRFD face
  detection, face-to-track association, recognition scheduling, matching
  (MatchEngine or FaceDB), interaction state machine, and match-event logging.
- **Build_System**: The `Makefile` that compiles sources under `src/` via the
  pattern rule `src/%.cpp -> build/%.o` and links each target.
- **Target_Toolchain**: The Linux build environment on the Orange Pi A733 board
  (or equivalent Linux host) with OpenCV4, VIPhal/NBGlinker from the AI SDK at
  `/home/orangepi/ai-sdk`, sqlite3, and pthread available.
- **Privacy_Rule_R7**: The existing project rule that resident names,
  `greeting_name`, and apartment identifiers are never written to logs; only
  numeric `resident_id` and `track_id` are logged.

## Requirements

### Requirement 1: Behavioral Equivalence

**User Story:** As a maintainer, I want the refactored application to behave
identically to the original, so that the refactor introduces no functional
regression.

#### Acceptance Criteria

1. WHEN the Refactored_App is run with any argument set accepted by the
   Baseline_App over the same ordered sequence of input frames, THE
   Refactored_App SHALL produce Pipeline_Behavior outcomes identical to the
   Baseline_App, where identical Pipeline_Behavior means the same per-frame set
   of detected faces, person tracks, and match/unknown recognition outcomes, and
   the same process exit code.
2. THE Refactored_App SHALL load the NPU models in the same order used by the
   Baseline_App: recognizer first (when a recognition model is provided), then
   the SCRFD face-detection context, then the YOLO person context (when a person
   model is provided).
3. THE Refactored_App SHALL return the same process exit codes as the
   Baseline_App for each outcome: 0 on normal completion, 1 on camera-source
   open failure, 2 on recognizer or resident-database initialization failure, 3
   on SCRFD detector initialization failure, and 4 on YOLO person-model creation
   or initialization failure.
4. THE Refactored_App SHALL run with the same threading model as the
   Baseline_App, comprising exactly one capture thread, exactly one display
   thread, and the main pipeline thread.
5. IF the Refactored_App receives a SIGINT or SIGTERM signal, THEN THE
   Refactored_App SHALL perform the same shutdown sequence as the Baseline_App,
   in this observable order: emit the benchmark summary to standard output, stop
   and join the capture thread and release the camera source, stop and join the
   display thread and destroy the display window, close the resident database
   when operational mode is active, destroy the loaded NPU contexts, and return
   exit code 0.
6. WHEN the q key or ESC key is pressed while the display window has focus, THE
   Refactored_App SHALL stop the pipeline and perform the same shutdown sequence
   as the Baseline_App, returning exit code 0.
7. WHEN the Refactored_App reaches the frame count given by the frames argument,
   THE Refactored_App SHALL stop the pipeline and perform the same shutdown
   sequence as the Baseline_App, returning exit code 0.

### Requirement 2: CLI Surface Preservation

**User Story:** As an operator, I want every command-line flag and default to
work exactly as before, so that existing launch scripts and habits keep working.

#### Acceptance Criteria

1. THE Config_Module SHALL accept every positional argument and option flag
   defined in the CLI_Surface of the Baseline_App, consuming at most 2 positional
   arguments (detect model path, then camera id) and ignoring any additional
   positional argument without error, matching the Baseline_App.
2. THE Config_Module SHALL apply, for each argument, the same default value as
   the Baseline_App, and WHERE the argument is absent THE Config_Module SHALL
   retain that default value.
3. WHEN a log-configuration argument (log level or log directory) is parsed, THE
   Config_Module SHALL apply the same precedence order as the Baseline_App, where
   a command-line value overrides the corresponding environment value and the
   environment value overrides the default value.
4. WHERE an argument has no environment-variable source in the Baseline_App, THE
   Config_Module SHALL derive that argument's effective value only from the
   command-line value when supplied and otherwise from the default value,
   matching the Baseline_App.
5. WHEN the -h flag or the --help flag is provided, THE Config_Module SHALL emit
   usage text byte-for-byte equal to the Baseline_App usage text on the same
   output stream (standard error), AND THE Refactored_App SHALL then terminate
   with process exit code 0, matching the Baseline_App.
6. WHERE a flag implicitly enables another setting in the Baseline_App (including
   enabling the person tracker when a person-model path is supplied, enabling
   recognition when a recognition-model path is supplied, enabling stream mode
   when a source URL or custom pipeline is supplied, and giving the resident
   database precedence over the face database when both are supplied), THE
   Config_Module SHALL reproduce the same implicit effect.
7. IF an unrecognized flag is supplied, or a value-taking flag is supplied
   without a following value, or a numeric flag is supplied a non-numeric value,
   THEN THE Config_Module SHALL handle it exactly as the Baseline_App does,
   ignoring the affected argument without emitting an error and without changing
   the Refactored_App exit code, and continuing to parse remaining arguments.

### Requirement 3: Log and Benchmark Output Preservation

**User Story:** As an operator monitoring the cabin, I want log lines and the
benchmark report to be unchanged, so that log parsing tools and dashboards
continue to work.

#### Acceptance Criteria

1. WHEN a given input triggers a code path that emits log lines in the
   Baseline_App, THE Refactored_App SHALL emit the same log lines for that code
   path with identical tag, level, and format string, and in the same relative
   order as the Baseline_App.
2. THE Refactored_App SHALL emit each Log_Output line character-for-character
   identical to the corresponding Baseline_App line, excluding fields whose
   values are timestamps or run-specific identifiers assigned at runtime.
3. THE Benchmark_Module SHALL produce a benchmark summary table with the same
   column layout, row labels, column ordering, and numeric format specifiers as
   the Baseline_App.
4. THE Refactored_App SHALL comply with Privacy_Rule_R7 by logging resident
   identity only as numeric resident_id and track_id in Log_Output.
5. THE Refactored_App SHALL comply with Privacy_Rule_R7 by excluding resident
   names, greeting_name, and apartment identifiers from Log_Output.
6. WHERE a log site is emitted only at DEBUG level in the Baseline_App, THE
   Refactored_App SHALL emit that log site only at DEBUG level.

### Requirement 4: Module Extraction and Cohesion

**User Story:** As a developer, I want each responsibility isolated in its own
module, so that the code is easier to read, review, and modify.

#### Acceptance Criteria

1. THE Video_IO_Module SHALL provide the threaded video capture and threaded
   display responsibilities such that, after extraction, these responsibilities
   are defined only within the Video_IO_Module and are not defined within the
   Main_Module.
2. THE Overlay_Module SHALL provide the UI scaling and overlay/HUD drawing
   responsibilities such that, after extraction, these responsibilities are
   defined only within the Overlay_Module and are not defined within the
   Main_Module.
3. THE Config_Module SHALL provide the CLI parsing, positional argument handling,
   and usage-text responsibilities such that, after extraction, these
   responsibilities are defined only within the Config_Module and are not defined
   within the Main_Module.
4. THE Benchmark_Module SHALL provide the stage timing accounting and benchmark
   summary responsibilities such that, after extraction, these responsibilities
   are defined only within the Benchmark_Module and are not defined within the
   Main_Module.
5. THE Main_Module SHALL contain exactly the model initialization and
   Pipeline_Behavior orchestration responsibilities, and SHALL NOT contain the
   video capture, display, UI scaling, overlay/HUD drawing, CLI parsing,
   positional argument handling, usage-text, stage timing accounting, or
   benchmark summary responsibilities.
6. WHEN the Build_System compiles the Main_Module, THE Build_System SHALL resolve
   every extracted-module responsibility used by the Main_Module through that
   module's header interface, with no responsibility redefined within the
   Main_Module.
7. WHERE a symbol is used by more than one binary in the Build_System, THE
   refactor SHALL place that symbol such that every binary that linked it before
   the refactor compiles and links successfully after the refactor.

### Requirement 5: Build System Integration

**User Story:** As a developer building on the target board, I want the Makefile
to compile the new modules cleanly, so that the build continues to succeed.

#### Acceptance Criteria

1. WHEN the Build_System compiles a new module source file through the existing
   `src/%.cpp -> build/%.o` pattern rule, THE Build_System SHALL produce the
   corresponding object file and terminate the compile step with a zero exit
   status.
2. WHEN the Build_System links a target that requires a new module, THE
   Build_System SHALL include that module's object file, resolve all symbols with
   zero unresolved-symbol errors, and terminate the link step with a zero exit
   status.
3. THE Build_System SHALL link the Refactored_App against the same set of
   libraries as the Baseline_App, adding no library and removing no library.
4. WHEN the Refactored_App is compiled on the Target_Toolchain with the warning
   flags already applied by the Build_System, THE Build_System SHALL report no
   compiler warning that the Baseline_App did not already report.
5. IF a new module source fails to compile or a required symbol is unresolved at
   link time, THEN THE Build_System SHALL terminate with a non-zero exit status
   and SHALL NOT produce the Refactored_App executable.

### Requirement 6: Verification Environment Constraint

**User Story:** As a developer working on Windows, I want the acceptance criteria
to account for my inability to run the Linux build locally, so that the spec is
achievable in my environment.

#### Acceptance Criteria

1. THE spec SHALL define the refactor acceptance goal as compilation on the
   Target_Toolchain with zero errors and zero new warnings and runtime output
   byte-for-byte identical to the Baseline_App across all executed code paths.
2. WHERE the developer environment is Windows, THE spec SHALL mark `make` build
   verification and runtime verification as blocked-pending-Linux rather than
   failed.
3. THE refactor SHALL be reviewable on the developer environment before Linux
   verification by line-by-line comparison of the extracted logic, control flow,
   and function signatures against the Baseline_App.
4. IF the static comparison on the developer environment detects any difference
   in logic, control flow, or function signature relative to the Baseline_App,
   THEN the difference SHALL be resolved or explicitly documented as intentional
   before Linux verification.
5. WHEN the refactor is verified on a Linux environment, THE verification SHALL
   confirm the acceptance goal defined in criterion 1 before the refactor is
   considered complete.
