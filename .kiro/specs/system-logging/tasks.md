# Tasks — System / Operational Logging

**Spec ID**: `system-logging`

- [ ] 1. Logger core (`src/log/logger.h` + `src/log/logger.cpp`)
  - [ ] 1.1 Khai báo API + macro trong `logger.h`: `LogLevel`, `LogConfig`, class `Logger` (singleton), macro `LOG_TRACE/DEBUG/INFO/WARN/ERROR` (check `enabled()` trước khi format). Tên KHÔNG trùng shim `log/log.h`.
    - _Requirements: R1, R2, R6.2_
  - [ ] 1.2 `logger.cpp`: singleton, `init()`, `log()/vlog()` với timestamp ms + level + tag, mutex ghi, sink stderr (màu khi TTY) + sink file.
    - _Requirements: R1, R2, R3, R6.1_
  - [ ] 1.3 Rotation theo ngày: đặt tên `basename-YYYY-MM-DD.log`; đổi file khi sang ngày mới; `cleanup_old()` xóa file quá `retention_days`.
    - _Requirements: R4_
  - [ ] 1.4 Fallback thư mục: default `/var/log/face-cabin`; nếu không ghi được → `./logs` + cảnh báo; vẫn lỗi → chỉ stderr. Không crash.
    - _Requirements: R5.1, R5.2_

- [ ] 2. Resolve cấu hình (CLI + env)
  - [ ] 2.1 Helper `resolve_log_config(argc, argv, defaults)`: áp env `FACE_CABIN_LOG_LEVEL`/`FACE_CABIN_LOG_DIR` rồi CLI `--log-level`/`--log-dir` (CLI > env > default). Truyền `--log-dir` cho tool → bật `to_file`.
    - _Requirements: R5.3, R5.4, R5.5, R8.2_
  - [ ] 2.2 `level_from_string()` / `level_name()`.
    - _Requirements: R1, R5.4_

- [ ] 3. Unit test logger (`tests/test_logger.cpp`, máy dev)
  - [ ] 3.1 Level filtering + định dạng dòng (regex) + đếm dòng ghi file tạm.
    - _Requirements: R1, R2_
  - [ ] 3.2 Rotation theo ngày + retention: tạo file ngày cũ → cleanup xóa đúng, giữ file trong hạn.
    - _Requirements: R4_
  - [ ] 3.3 Fallback dir không ghi được → không crash, chuyển `./logs`.
    - _Requirements: R5.2_
  - [ ] 3.4 Thread-safe: nhiều thread cùng log → không xen dòng, đủ số dòng.
    - _Requirements: R6.1_
  - [ ] 3.5 Thêm vào `tests/run_tests.sh`.
    - _Requirements: NFR build_

- [ ] 4. Build (Makefile)
  - [ ] 4.1 Thêm `src/log/logger.cpp` vào `COMMON_SRCS` (mọi binary tự có logger).
    - _Requirements: NFR header-only/build_
  - [ ] 4.2 Xác nhận build cả 4 (5) binary pass; thêm `-lstdc++fs` cho `main` nếu link `<filesystem>` báo thiếu.
    - _Requirements: NFR build_

- [ ] 5. Di trú `main.cpp` sang logger
  - [ ] 5.1 Init logger đầu `main` với defaults app chính (to_file, `/var/log/face-cabin`); parse `--log-level`/`--log-dir`; cập nhật usage.
    - _Requirements: R8.1, R5.3, R5.4_
  - [ ] 5.2 Thay `printf`/`fprintf` log → `LOG_*` với tag (`cam`,`npu`,`detect`,`recog`,`track`,`db`,`ui`). Per-frame bench/recog log → `LOG_DEBUG`. Giữ `print_usage` dạng `fprintf`. HUD overlay giữ nguyên.
    - _Requirements: R8.1, R8.3, R7, NFR không sụt FPS_

- [ ] 6. Di trú các tool sang logger
  - [ ] 6.1 `enroll_faces.cpp`, `add_person.cpp`, `capture_person.cpp` (+ `migrate_fdb.cpp` khi có): init logger defaults tool (stderr-only, bật file nếu `--log-dir`); thay log `printf`/`fprintf` → `LOG_*`; giữ `print_usage`.
    - _Requirements: R8.2, R8.3, R7_

- [ ] 7. Kiểm thử tích hợp
  - [ ] 7.1 Chạy `face_recog_app` vài giây → có file `face-cabin-<hôm nay>.log` ở `/var/log/face-cabin` (hoặc `./logs` khi thiếu quyền); nội dung đúng định dạng.
    - _Requirements: R2, R3, R4, R5_
  - [ ] 7.2 `--log-level debug` thấy log per-frame; `--log-level warn` im per-frame.
    - _Requirements: R1_
  - [ ] 7.3 Rà PII: đọc file log xác nhận không có tên/greeting_name/apartment.
    - _Requirements: R7_
