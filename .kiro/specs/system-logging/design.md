# Design — System / Operational Logging

**Spec ID**: `system-logging`

## 1. Tổng quan

Một logger singleton, thread-safe, gồm:
- **Core** (`Logger`): giữ cấu hình (level, dir, sink flags), mutex, file handle của ngày hiện tại.
- **Sinks**: stderr (luôn có) + file theo ngày (tùy chọn).
- **Rotation theo ngày**: khi ghi mà ngày (YYYY-MM-DD) khác ngày của file đang mở → đóng file cũ, mở file mới, dọn file quá hạn.
- **Macro tiện dụng**: `LOG_INFO(tag, fmt, ...)` v.v., kiểm tra level TRƯỚC khi format để bản ghi bị lọc gần như miễn phí.

Chọn **tách 1 `.cpp`** (`src/log/logger.cpp`) cho phần định nghĩa singleton + state, còn `src/log/logger.h` khai báo API + macro. Lý do: singleton có state (file handle, mutex) không nên định nghĩa trong header (tránh multiple-definition khi nhiều TU include). Đây vẫn "gần header-only" về mặt sử dụng.

> Lưu ý tên file: KHÔNG đặt trùng `src/log/log.h` (shim SDK NPU). Dùng `src/log/logger.h` + `src/log/logger.cpp`.

## 2. API

```cpp
// src/log/logger.h
#pragma once
#include <cstdarg>
#include <string>

enum class LogLevel { TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, OFF=5 };

struct LogConfig {
    LogLevel    level        = LogLevel::INFO;
    bool        to_stderr    = true;
    bool        to_file      = false;         // app chính bật; tool để false
    std::string dir          = "/var/log/face-cabin";
    std::string basename     = "face-cabin";  // → face-cabin-YYYY-MM-DD.log
    int         retention_days = 14;
    bool        color        = true;          // chỉ áp dụng khi stderr là TTY
};

class Logger {
public:
    static Logger& instance();

    // Khởi tạo 1 lần ở đầu main. Áp CLI/env đã resolve sẵn (xem §4).
    // Nếu to_file mà không mở được dir → fallback ./logs + cảnh báo (R5.2).
    void init(const LogConfig& cfg);

    void set_level(LogLevel lv);
    LogLevel level() const;          // dùng để check trước khi format
    bool     enabled(LogLevel lv) const;

    // Ghi 1 bản ghi đã format sẵn (macro gọi hàm này).
    void log(LogLevel lv, const char* tag, const char* fmt, ...);
    void vlog(LogLevel lv, const char* tag, const char* fmt, va_list ap);

    void flush();
    void shutdown();                 // đóng file (gọi cuối main; không bắt buộc)

    // Helpers cấu hình.
    static LogLevel level_from_string(const std::string& s); // "info" → INFO
    static const char* level_name(LogLevel lv);              // INFO → "INFO "
};

// ---- Macro: check level trước để tránh chi phí format khi bị lọc ----
#define LOG_AT(lv, tag, ...)                                        \
    do {                                                           \
        Logger& _lg = Logger::instance();                          \
        if (_lg.enabled(lv)) _lg.log((lv), (tag), __VA_ARGS__);    \
    } while (0)

#define LOG_TRACE(tag, ...) LOG_AT(LogLevel::TRACE, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) LOG_AT(LogLevel::DEBUG, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  LOG_AT(LogLevel::INFO,  tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  LOG_AT(LogLevel::WARN,  tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) LOG_AT(LogLevel::ERROR, tag, __VA_ARGS__)
```

## 3. Cơ chế bên trong (`logger.cpp`)

**State**: `LogConfig cfg_`, `std::mutex mtx_`, `FILE* fp_` (file ngày hiện tại), `std::string cur_date_` (YYYY-MM-DD của `fp_`).

**`log()` / `vlog()`**:
1. (Đã qua check `enabled()` ở macro; kiểm tra lại phòng gọi trực tiếp.)
2. Lấy `now` (`std::chrono::system_clock`), tách ra ngày `YYYY-MM-DD` + giờ `HH:MM:SS.mmm` (dùng `localtime_r`).
3. `lock_guard(mtx_)`.
4. Nếu `to_file` và `date != cur_date_` → `rotate_to(date)` (đóng fp_ cũ, mở `dir/basename-date.log` chế độ append, cập nhật `cur_date_`, gọi `cleanup_old()`).
5. Format prefix `"<ts> [<LEVEL>] [<tag>] "` rồi `vsnprintf` phần message vào buffer stack (vd 1024B; nếu tràn thì cấp phát động).
6. Ghi ra stderr (kèm màu nếu `cfg_.color && isatty(fileno(stderr))`), ghi ra `fp_` (không màu) nếu bật; `fflush(fp_)` để không mất log khi crash (chấp nhận đánh đổi I/O — log không phải per-frame nóng).

**`rotate_to(date)`**: mở file mới; nếu mở lỗi → tắt `to_file`, log 1 cảnh báo ra stderr (không lặp).

**`cleanup_old()`**: quét `dir` các file khớp `basename-YYYY-MM-DD.log`, parse ngày từ tên, xóa file cũ hơn `retention_days` so với hôm nay. Bọc try/catch quanh `<filesystem>` để lỗi FS không làm chết app.

**Màu (tùy chọn)**: TRACE xám, DEBUG xanh dương, INFO mặc định, WARN vàng, ERROR đỏ (ANSI). Chỉ khi stderr là TTY.

## 4. Resolve cấu hình (thứ tự ưu tiên R5.5)

Hàm free `LogConfig resolve_log_config(argc, argv, defaults)`:
- Bắt đầu từ `defaults` (khác nhau giữa app chính vs tool — xem §6).
- Áp env: `FACE_CABIN_LOG_LEVEL`, `FACE_CABIN_LOG_DIR` nếu có.
- Áp CLI: `--log-level`, `--log-dir` (ghi đè env).
- Trả về config đã hợp nhất → `Logger::instance().init(cfg)`.

Các tool đã có vòng lặp parse args riêng; thêm nhận diện `--log-level`/`--log-dir` vào đó (hoặc tách 1 helper chung dùng lại).

## 5. Fallback thư mục (R5.2)

Trong `init()` khi `to_file`:
1. `std::filesystem::create_directories(cfg_.dir)` trong try/catch.
2. Thử mở file ngày hiện tại. Nếu OK → dùng.
3. Nếu lỗi (permission) → đặt `cfg_.dir = "./logs"`, tạo lại, in cảnh báo ra stderr: `"[log] cannot use <dir>, falling back to ./logs"`. Nếu vẫn lỗi → tắt `to_file` (chỉ stderr), cảnh báo 1 lần.

## 6. Mặc định theo binary (R8)

| Binary | to_file | dir mặc định | Ghi chú |
|---|---|---|---|
| `face_recog_app` (main) | true | `/var/log/face-cabin` | dịch vụ 24/7; fallback `./logs` |
| `enroll_faces` | false | (n/a) | chỉ stderr; bật file nếu truyền `--log-dir` |
| `add_person` | false | (n/a) | như trên |
| `capture_person` | false | (n/a) | như trên |
| `migrate_fdb` | false | (n/a) | như trên |

Quy tắc: truyền `--log-dir` cho tool → tự động bật `to_file`.

## 7. Di trú printf → logger (R8.3)

- Thay `printf("[cam] ...")` → `LOG_INFO("cam", ...)`, `fprintf(stderr, "[xxx] ...")` cảnh báo → `LOG_WARN`/`LOG_ERROR` với tag tương ứng.
- Các dòng per-frame nóng trong `main.cpp` (bench `[frame N] fps=...`, `[recog] frame ...`) → hạ xuống `LOG_DEBUG` để không spam file ở INFO. HUD overlay trên ảnh giữ nguyên (không phải log).
- Giữ nguyên `fprintf(stderr, ...)` trong hàm `print_usage()` (in help CLI — không phải log ứng dụng, R8.3 cho phép).
- KHÔNG đụng `src/log/log.h` (shim SDK).

## 8. Thay đổi build (Makefile)

- Thêm `src/log/logger.cpp` vào **tất cả** nhóm nguồn: `COMMON_SRCS` (dùng chung cho mọi binary) là chỗ hợp lý nhất → mọi target tự có logger.
- `-I$(SRC_DIR)` đã có nên `#include "log/logger.h"` resolve được.
- Kiểm tra `<filesystem>`: g++ ≥ 9 không cần `-lstdc++fs`; các tool đã link `-lstdc++fs` sẵn (enroll/capture/add) nên an toàn. Nếu link `main` báo thiếu thì thêm `-lstdc++fs`.

## 9. Kiểm thử

- **Unit (máy dev / WSL, không NPU)** — `tests/test_logger.cpp`:
  - Level filtering: set WARN → DEBUG/INFO không ghi, WARN/ERROR có ghi (ghi ra file tạm rồi đọc lại đếm dòng).
  - Định dạng: dòng khớp regex `^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[(TRACE|DEBUG|INFO|WARN|ERROR)\] \[tag\] msg`.
  - Rotation theo ngày: giả lập bằng cách cho phép inject "ngày" (thêm hook test) HOẶC kiểm tra tên file chứa ngày hôm nay + tạo file ngày cũ rồi gọi cleanup → bị xóa.
  - Fallback dir: init với dir không ghi được (vd đường dẫn cấm) → không crash, chuyển `./logs`.
  - Thread-safe: nhiều thread cùng log → không có dòng bị xen (mỗi dòng nguyên vẹn, đúng số dòng).
  - Retention: tạo file cũ giả (đổi tên có ngày quá hạn) → cleanup xóa đúng, giữ file trong hạn.
- **Tích hợp**: chạy `face_recog_app` vài giây → có file `/var/log/face-cabin/face-cabin-<hôm nay>.log` (hoặc `./logs/` khi thiếu quyền); `--log-level debug` thấy log per-frame; `--log-level warn` im per-frame.

## 10. Rủi ro

| Rủi ro | Giảm thiểu |
|---|---|
| `fflush` mỗi bản ghi làm chậm khi log dày | Log dày (per-frame) để ở DEBUG/TRACE, mặc định INFO nên I/O thấp; cân nhắc bỏ flush ở TRACE nếu cần. |
| `<filesystem>` link lỗi trên toolchain cũ | Thử build; thêm `-lstdc++fs` nếu cần (đã dùng ở tool khác). |
| Thiếu quyền `/var/log` | Fallback `./logs` + cảnh báo (R5.2). |
| Đổi ngày lúc nửa đêm khi đang tải cao | Rotation nằm trong cùng mutex với ghi; chi phí 1 lần/ngày, không đáng kể. |
| Lỡ ghi PII vào log | Review khi di trú; quy ước chỉ dùng `resident_id`/`track_id` (R7). |
