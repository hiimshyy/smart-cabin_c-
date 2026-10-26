# AGENTS.md

This file provides guidance to the AI agent when working with code in this repository.

## Build and test

- Run all commands from the repo root: model paths, `db/schema.sql`, `logs/`, `recordings/` are relative.
- `make -j4` builds six binaries: `face_recog_app`, `enroll_faces`, `capture_person`, `add_person`, `probe_models`, `migrate_fdb`. `AI_SDK` is hardcoded to `/home/orangepi/ai-sdk`.
- `bash tests/run_tests.sh` builds and runs all tests (custom harness, no framework). Each test is a standalone `main()` registered manually in that script; outputs go to `/tmp/scabin_tests`. To run one test, copy its `g++` block from `run_tests.sh`.
- SQLite-backed tests take `db/schema.sql` as `argv[1]` and are skipped (as failures) without sqlite3 headers; fallback is `bash tests/_setup_sqlite_local.sh`.
- Style tooling: `make format-check` (clang-format, config in `.clang-format`) and `make lint` (clang-tidy on app sources, config in `.clang-tidy`). Neither is part of `all`; both need `clang-format`/`clang-tidy` installed via apt. Format only files you touched — never reformat the whole repo inside a feature change. Full `make lint` takes minutes, so lint the changed file during iteration (`clang-tidy <file>.cpp -- -std=c++17 -Isrc $(pkg-config --cflags opencv4)`).

## Hard rules

- Never modify `src/log/log.h` — it is the NPU SDK log-mute shim, not the app logger. Use `src/log/logger.h` for app logging.
- Never log PII: no resident names, `greeting_name`, apartment, embedding vectors, or image paths. Log numeric IDs only.
- Keep the main frame loop non-blocking: network, disk logging, recording, and DB writes go through worker threads with bounded drop-oldest queues. Swap catalogs only at frame boundaries.
- Config precedence is CLI > SQLite `cabins` table > compile default; changes apply on restart (no hot reload). Note `env.sh` always passes `--source` resolved from PostgreSQL, which overrides `cabins.camera_urls`.
- Edge Core UDS (`--edge-socket`) is legacy and superseded by Socket.IO; do not extend its 8-byte framing.
- Never commit runtime data under `faces/`, `recordings/`, `logs/`, `db/*.db*`, or `model/person_det/` (gitignored; `.nb` files are not fetched by `make`).

## Style

- 4 spaces, `#pragma once`, no project-wide namespace; helpers go in an anonymous namespace.
- Types `PascalCase`; functions/fields `snake_case`; constants `SCREAMING_SNAKE`.
- Errors via `bool` return + log + optional `std::string& error`; exceptions are rare.
- Code, comments, log messages, and commits in English. README and `.kiro/specs/` docs in Vietnamese.
- Register every new test in `tests/run_tests.sh`.
- When implementing a spec, update its `.kiro/specs/<name>/tasks.md` and `design.md` alongside the code.
- Commit directly to `main` using Conventional Commits (lowercase, imperative, e.g. `fix(tracker): ...`).

## Gotchas

- `SIGINT` is deliberately ignored (`SIG_IGN`); only `SIGTERM` stops the app. The systemd unit uses `KillSignal=SIGINT`, so `systemctl stop` waits for `TimeoutStopSec` then SIGKILLs.
- The NPU has one core shared by the app and enroll/add tools; model load order is recog -> SCRFD -> YOLO to reduce NBG resource conflicts. Contexts are never unloaded at runtime.
- `AppConfig` compiles with `TrackerMode::Greedy`, but `env.sh` exports `FACE_TRACKER_MODE=byte`.
- `face-recog-app.service` launches via `source env.sh && face_cabin`; the same env is what production uses.
