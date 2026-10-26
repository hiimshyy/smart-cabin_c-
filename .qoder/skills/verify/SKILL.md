---
name: verify
description: Build the six binaries and run the full custom test suite. Use before reporting a code change as done.
---

Verify this repo's changes from the repo root (`/home/orangepi/face_recog_app`).

1. Build: `make -j4`. If it fails, report the first compile/link error and stop — do not run tests against a stale build.
2. Test: `bash tests/run_tests.sh`. It builds each test with its own `g++` line and runs it; all binaries go to `/tmp/scabin_tests`.
3. Report results:
   - Name each failing test binary and quote its `FAIL: ... (line N)` lines.
   - If SQLite-dependent tests were skipped ("FAILED (no sqlite3 headers/library)"), say so explicitly instead of reporting a clean pass; the fix is `sudo apt-get install -y libsqlite3-dev` or `bash tests/_setup_sqlite_local.sh`.
   - End with `ALL TESTS PASSED` only if the script actually printed it.
4. If the change touches one module and you want a faster loop, run just that test's `g++` block from `tests/run_tests.sh`, but run the full suite before declaring done.

Skip all of this for docs/spec-only changes (e.g. `README.md`, `.kiro/specs/`); state that you skipped verification and why.
