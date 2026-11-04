# Implementation Plan: System / Operational Logging (system-logging)

## Overview

**Spec ID**: `system-logging`
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

Kế hoạch triển khai logger singleton, thread-safe, tự viết (`src/log/logger.h` + `src/log/logger.cpp`) dùng chung cho mọi binary: sink stderr + file theo ngày, retention, fallback thư mục, cấu hình CLI > env > mặc định. Sau đó di trú các `printf`/`fprintf` dùng để log trong `main.cpp` và 4 tool sang logger.

Thứ tự: logger core và resolve cấu hình (task 1–2), unit test (task 3), build (task 4), di trú `main.cpp` và các tool (task 5–6), rồi kiểm thử tích hợp trên Orange Pi (task 7).

### 📌 TRẠNG THÁI (để tiếp tục trên Orange Pi)

**Đã xong (test pass trên máy dev / WSL):**
- ✅ **Task 1** — Logger core `src/log/logger.h` + `src/log/logger.cpp`.
- ✅ **Task 2** — `resolve_log_config()` + `level_from_string()`/`level_name()` (làm luôn trong `logger.cpp`).
- ✅ **Task 3** — Unit test `tests/test_logger.cpp` (**14 checks pass**) + đã thêm vào `tests/run_tests.sh`.

**Đã xong trên Orange Pi (build đầy đủ + camera/NPU):**
- ✅ **Task 4** (commit `d46384c`) — `logger.cpp` vào `COMMON_SRCS` + `MIGRATE_SRCS_CPP`; cả 6 binary build; g++ 12.2 không cần `-lstdc++fs`.
- ✅ **Task 5** (commit `5672530`) — di trú `main.cpp`: init logger đầu main (to_file, `/var/log/face-cabin`→fallback `./logs`), tags cam/recog/db/detect/yolo/track/ui/event/main, per-frame → DEBUG, event → INFO không PII, giữ print_usage + bench summary.
- ✅ **Task 6** (commit `633064e`) — di trú 4 tool (enroll/add/capture/migrate): stderr-only mặc định, bật file qua `--log-dir`; parser strict (add/migrate) nuốt `--log-level`/`--log-dir`; PII sạch (resident_id + basename ảnh).
- ✅ **Task 7** — kiểm thử tích hợp trên Orange Pi: file `face-cabin-<ngày>.log` đúng tên, 0 dòng sai format regex, `--log-level debug` hiện per-frame, `warn` im, PII audit sạch.

**TOÀN BỘ SPEC HOÀN TẤT (Task 1-7).**

**Cách chạy unit test:**
- Orange Pi (checkout LF): `bash tests/run_tests.sh`.
- Máy Windows/WSL (file bị CRLF do OneDrive): `wsl bash -c "cd '<repo>' && tr -d '\r' < tests/run_tests.sh > tests/.run_tests_lf.sh && bash tests/.run_tests_lf.sh; rm -f tests/.run_tests_lf.sh"`.

**Quyết định thiết kế đã chốt:**
- Logger tự viết, KHÔNG spdlog; rotation THEO NGÀY; retention mặc định 14 ngày.
- App chính: `to_file=true`, dir mặc định `/var/log/face-cabin`, fallback `./logs`. Tool: chỉ stderr, bật file khi truyền `--log-dir`.
- Cấu hình: CLI `--log-level`/`--log-dir` > env `FACE_CABIN_LOG_LEVEL`/`FACE_CABIN_LOG_DIR` > default.
- KHÔNG đụng `src/log/log.h` (shim SDK NPU). Logger dùng `src/log/logger.h`.
- Không PII trong log: dùng `resident_id`/`track_id`, không ghi tên/greeting_name/apartment.

## Tasks

- [x] 1. Logger core (`src/log/logger.h` + `src/log/logger.cpp`)

  - [x] 1.1 Khai báo API + macro trong `logger.h`: `LogLevel`, `LogConfig`, class `Logger` (singleton), macro `LOG_TRACE/DEBUG/INFO/WARN/ERROR` (check `enabled()` trước khi format). Tên KHÔNG trùng shim `log/log.h`.
    - _Requirements: R1, R2, R6.2_
  - [x] 1.2 `logger.cpp`: singleton, `init()`, `log()/vlog()` với timestamp ms + level + tag, mutex ghi, sink stderr (màu khi TTY) + sink file.
    - _Requirements: R1, R2, R3, R6.1_
  - [x] 1.3 Rotation theo ngày: đặt tên `basename-YYYY-MM-DD.log`; đổi file khi sang ngày mới; `cleanup_old()` xóa file quá `retention_days`.
    - _Requirements: R4_
  - [x] 1.4 Fallback thư mục: default `/var/log/face-cabin`; nếu không ghi được → `./logs` + cảnh báo; vẫn lỗi → chỉ stderr. Không crash.
    - _Requirements: R5.1, R5.2_
- [x] 2. Resolve cấu hình (CLI + env)  ✅ (làm chung trong `logger.cpp`)

  - [x] 2.1 Helper `resolve_log_config(argc, argv, defaults)`: env `FACE_CABIN_LOG_LEVEL`/`FACE_CABIN_LOG_DIR` rồi CLI `--log-level`/`--log-dir` (CLI > env > default). `--log-dir` → bật `to_file`.
    - _Requirements: R5.3, R5.4, R5.5, R8.2_
  - [x] 2.2 `level_from_string()` / `level_name()`.
    - _Requirements: R1, R5.4_
- [x] 3. Unit test logger (`tests/test_logger.cpp`, máy dev)  ✅ 14 checks pass

  - [x] 3.1 Level filtering + định dạng dòng (regex) + đếm dòng ghi file tạm.
    - _Requirements: R1, R2_
  - [x] 3.2 Rotation theo ngày + retention: tạo file ngày cũ (2000-*) → cleanup xóa; file -3 ngày (trong hạn) + file hôm nay giữ; file non-log để nguyên.
    - _Requirements: R4_
  - [x] 3.3 Fallback dir không ghi được → không crash, chuyển `./logs`.
    - _Requirements: R5.2_
  - [x] 3.4 Thread-safe: 6 thread × 200 dòng → không xen, đủ 1200 dòng.
    - _Requirements: R6.1_
  - [x] 3.5 Thêm vào `tests/run_tests.sh`.
    - _Requirements: NFR build_
- [x] 4. Build (Makefile)  ✅ (commit `d46384c`)

  - [x] 4.1 Thêm `src/log/logger.cpp` vào `COMMON_SRCS` + `MIGRATE_SRCS_CPP` (mọi binary tự có logger).
    - _Requirements: NFR header-only/build_
  - [x] 4.2 Build cả 6 binary pass; g++ 12.2 Orange Pi không cần `-lstdc++fs`.
    - _Requirements: NFR build_
- [x] 5. Di trú `main.cpp` sang logger  ✅ (commit `5672530`)

  - [x] 5.1 Init logger đầu `main` với defaults app chính (to_file, `/var/log/face-cabin`); `resolve_log_config` đọc `--log-level`/`--log-dir` + env; usage cập nhật.
    - _Requirements: R8.1, R5.3, R5.4_
  - [x] 5.2 Thay `printf`/`fprintf` log → `LOG_*` (tag cam/npu/detect/recog/track/db/ui/event/main). Per-frame bench/recog → `LOG_DEBUG`. Giữ `print_usage` + bench summary. HUD overlay giữ nguyên. Không PII (resident_id/track_id).
    - _Requirements: R8.1, R8.3, R7, NFR không sụt FPS_
- [x] 6. Di trú các tool sang logger  ✅ (commit `633064e`)

  - [x] 6.1 `enroll_faces`, `add_person`, `capture_person`, `migrate_fdb`: init logger defaults tool (stderr-only, bật file nếu `--log-dir`); thay log → `LOG_*`; giữ `print_usage` + lỗi CLI trước khi init logger. Parser strict (add/migrate) nuốt `--log-level`/`--log-dir`. Không PII: resident_id + basename ảnh, không log full path chứa tên folder.
    - _Requirements: R8.2, R8.3, R7_
- [x] 7. Kiểm thử tích hợp  ✅ (Orange Pi + USB cam)

  - [x] 7.1 Chạy `face_recog_app` → file `face-cabin-2026-09-07.log` ở `./logs` (fallback vì `/var/log` không ghi được ở user thường); tên khớp `face-cabin-YYYY-MM-DD.log`; 0 dòng sai format regex.
    - _Requirements: R2, R3, R4, R5_
  - [x] 7.2 `--log-level debug` → 6 dòng per-frame; `--log-level warn` → 0 dòng INFO/DEBUG.
    - _Requirements: R1_
  - [x] 7.3 Rà PII: grep tên/greeting trên toàn bộ log → không có.
    - _Requirements: R7_

## Notes

- Task 1–7 đã xong. Task Dependency Graph ghi phụ thuộc logic giữa các task, không phải thứ tự commit thực tế.
- Graph dựa trên API mà từng task dùng:
  - `logger.cpp` (1.2) định nghĩa API khai báo trong `logger.h` (1.1). Rotation (1.3) và fallback thư mục (1.4) mở rộng sink file và `init()` của 1.2.
  - `level_from_string()`/`level_name()` (2.2) dùng `LogLevel` (1.1). `resolve_log_config` (2.1) gọi `level_from_string()` để đọc `FACE_CABIN_LOG_LEVEL` và `--log-level`.
  - Unit test: 3.1 và 3.4 cần core 1.2; 3.2 cần rotation/retention 1.3; 3.3 cần fallback 1.4. 3.5 đưa `tests/test_logger.cpp` (đủ 3.1–3.4) vào `tests/run_tests.sh`.
  - Makefile 4.1 cần `src/log/logger.cpp` (1.2). Build 4.2 biên dịch toàn bộ `logger.cpp`, nên cần 1.3, 1.4 và 2.1.
  - Di trú `main.cpp` (5.1) và các tool (6.1) gọi `resolve_log_config` (2.1) và cần mọi binary đã link logger (4.2). 5.2 thay `printf`/`fprintf` sau khi logger được init ở 5.1.
  - Kiểm thử tích hợp 7.1 và 7.2 chạy `face_recog_app` đã di trú (5.2). Rà PII 7.3 cần cả `main.cpp` (5.2) và các tool (6.1) đã di trú.
- Một số task cùng wave sửa cùng file: 1.3, 1.4 và 2.1 (wave 2) cùng sửa `src/log/logger.cpp`; 3.1 và 3.4 (wave 2), 3.2 và 3.3 (wave 3) cùng sửa `tests/test_logger.cpp`. Các task này đã xong; nếu phải làm lại thì chạy tuần tự.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "2.2"] },
    { "id": 2, "tasks": ["1.3", "1.4", "2.1", "3.1", "3.4", "4.1"] },
    { "id": 3, "tasks": ["3.2", "3.3", "4.2"] },
    { "id": 4, "tasks": ["3.5", "5.1", "6.1"] },
    { "id": 5, "tasks": ["5.2"] },
    { "id": 6, "tasks": ["7.1", "7.2", "7.3"] }
  ]
}
```
