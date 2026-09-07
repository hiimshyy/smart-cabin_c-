# Requirements — System / Operational Logging (Loại A)

**Spec ID**: `system-logging`
**Loại**: Nhật ký kỹ thuật của tiến trình (KHÔNG phải audit nghiệp vụ — cái đó nằm ở `match_events`).
**Phụ thuộc**: không có (độc lập với `resident-db-layer`); áp dụng cho toàn bộ binary trong `src/`.

## 1. Bối cảnh & vấn đề

Hiện toàn bộ ứng dụng ghi log bằng `printf`/`fprintf(stderr, ...)` rải rác trong `main.cpp` và các tool. Vấn đề:

- Không có **level** → không lọc được noise vs lỗi quan trọng.
- Không ghi **file** → chạy 24/7 qua systemd, khi sự cố không có lịch sử để truy.
- Không **xoay vòng** → nếu ghi file sẽ đầy đĩa.
- Không **thread-safe** → app có capture thread + display thread + DB writer thread, `printf` xen kẽ nhau gây rối.
- `src/log/log.h` KHÔNG phải logger ứng dụng — nó chỉ là shim tắt log ồn của SDK NPU (awnn). Giữ nguyên, không đụng.

Cần một logger nhẹ, tự viết (không thêm dependency), cho thiết bị nhúng Orange Pi chạy dịch vụ liên tục.

## 2. Phạm vi

**Trong scope:**
1. Logger header-only tự viết (`src/log/logger.h`) — level, thread-safe, ghi stderr + file, rotation theo ngày.
2. Cấu hình qua CLI + biến môi trường.
3. Di trú các `printf`/`fprintf` hiện có sang logger ở **tất cả** binary: `main.cpp`, `enroll_faces.cpp`, `add_person.cpp`, `capture_person.cpp`, và `migrate_fdb.cpp` (khi tồn tại).

**Ngoài scope:**
- Audit nghiệp vụ (`match_events`) — thuộc `resident-db-layer`.
- Gửi log qua mạng / log tập trung (syslog remote, Loki...) — version sau.
- Shim SDK `src/log/log.h` — giữ nguyên.

## 3. Yêu cầu chức năng

### R1 — Log levels
1. HỆ THỐNG PHẢI hỗ trợ 5 mức theo thứ tự: `TRACE < DEBUG < INFO < WARN < ERROR`.
2. HỆ THỐNG PHẢI có ngưỡng lọc runtime; chỉ ghi bản ghi có level ≥ ngưỡng.
3. Ngưỡng mặc định PHẢI là `INFO`.

### R2 — Định dạng bản ghi
1. Mỗi dòng log PHẢI có: timestamp (đến millisecond), level, tag module, message.
   Ví dụ: `2026-09-03 14:22:01.123 [INFO ] [cam] opened /dev/video0 640x480`.
2. Timestamp PHẢI theo giờ địa phương của thiết bị (định dạng `YYYY-MM-DD HH:MM:SS.mmm`).
3. `tag` là chuỗi ngắn định danh module (vd `cam`, `npu`, `db`, `track`, `enroll`).

### R3 — Đích ghi (sinks)
1. HỆ THỐNG PHẢI ghi ra **stderr** (để systemd/journald thu được).
2. HỆ THỐNG PHẢI ghi ra **file** khi bật (mặc định bật cho app chính).
3. HỆ THỐNG PHẢI cho phép tắt sink file (chỉ stderr) qua cấu hình.
4. Bản ghi ra stderr KHI gắn TTY CÓ THỂ tô màu theo level; khi không phải TTY thì không màu.

### R4 — Xoay vòng theo ngày
1. File log PHẢI đặt tên theo ngày: `face-cabin-YYYY-MM-DD.log`.
2. KHI sang ngày mới (bản ghi đầu tiên của ngày), HỆ THỐNG PHẢI tự chuyển sang file mới.
3. HỆ THỐNG PHẢI xóa file log cũ hơn `retention_days` (mặc định 14 ngày).
4. Việc dọn file cũ KHÔNG được chặn luồng chính đáng kể (chạy 1 lần lúc khởi tạo và khi đổi ngày).

### R5 — Thư mục & cấu hình đường dẫn
1. Thư mục log mặc định PHẢI là `/var/log/face-cabin/`.
2. NẾU không tạo/ghi được thư mục mặc định (thiếu quyền), HỆ THỐNG PHẢI fallback về `./logs/` và in cảnh báo ra stderr, KHÔNG crash.
3. HỆ THỐNG PHẢI cho phép override thư mục qua CLI `--log-dir <dir>` và biến môi trường `FACE_CABIN_LOG_DIR`.
4. HỆ THỐNG PHẢI cho phép chỉnh level qua CLI `--log-level <trace|debug|info|warn|error>` và biến môi trường `FACE_CABIN_LOG_LEVEL`.
5. Thứ tự ưu tiên cấu hình: CLI > biến môi trường > mặc định.

### R6 — Thread-safe
1. HỆ THỐNG PHẢI serialize việc ghi (mutex) để dòng log từ nhiều thread không xen kẽ.
2. Logger PHẢI dùng được như singleton toàn cục (init 1 lần ở đầu `main`, gọi từ mọi nơi).

### R7 — Riêng tư (không PII)
1. Log kỹ thuật KHÔNG được ghi tên thật / `greeting_name` / `apartment` của cư dân.
2. KHI cần tham chiếu một người trong log, HỆ THỐNG PHẢI dùng `resident_id` (số) hoặc `track_id`.

### R8 — Di trú & cấu hình từng binary
1. `main.cpp` PHẢI dùng logger (bật file, mặc định `/var/log/face-cabin/`).
2. Các tool offline (`enroll_faces`, `add_person`, `capture_person`, `migrate_fdb`) PHẢI dùng logger; mặc định các tool này CHỈ ghi stderr (không đòi ghi `/var/log`), nhưng vẫn bật được file qua `--log-dir`.
3. Sau di trú, KHÔNG còn `printf`/`fprintf(stderr,...)` cho mục đích log ứng dụng trong các file trên (trừ phần in usage/help của CLI — được phép giữ `fprintf`).

## 4. Yêu cầu phi chức năng

- **Không dependency mới**: chỉ dùng C++17 std + POSIX (`<filesystem>`, `<mutex>`, `<chrono>`). Không spdlog.
- **Hiệu năng**: overhead mỗi bản ghi phải nhỏ; các bản ghi bị lọc (dưới ngưỡng) gần như miễn phí (kiểm tra level trước khi format).
- **Không làm sụt FPS**: tránh log level cao (INFO+) trong vòng lặp per-frame nóng; per-frame chi tiết để ở `DEBUG`/`TRACE`.
- **Header-only**: để mọi binary include mà không phải sửa nhiều Makefile (có thể tách 1 `.cpp` cho định nghĩa singleton nếu cần, xem design).
- **Build**: compile trên Orange Pi (arm64, g++) và máy dev (WSL). `<filesystem>` cần link `-lstdc++fs` trên vài toolchain cũ — kiểm tra.

## 5. Giả định

- Thiết bị có đồng hồ hệ thống đúng (dùng giờ địa phương). Không xử lý NTP ở đây.
- systemd sẽ thu stderr vào journald; file log là bản lưu bổ sung, không thay thế journald.
- Rotation theo ngày là đủ; không cần rotation theo kích thước ở v1 (nếu 1 ngày quá lớn sẽ cân nhắc sau).
