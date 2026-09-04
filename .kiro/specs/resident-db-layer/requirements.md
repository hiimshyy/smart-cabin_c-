# Requirements — Lớp SQLite + Multi-embedding + State Machine tương tác

**Spec ID**: `resident-db-layer` · **Đề xuất số**: 1 (nền tảng)
**Phụ thuộc**: pipeline vision hiện tại (`face_recog.h`, `face_db.h`, `tracker.h`, `main.cpp`, `db/schema.sql`)

## 1. Bối cảnh & vấn đề

Hệ thống hiện nhận diện tốt (~25 FPS) nhưng lớp dữ liệu chỉ là file `.fdb`:

- `.fdb` lưu **1 embedding trung bình/người** — mất thông tin đa góc/ánh sáng, kém với ảnh thẻ 3×4.
- Không có `home_floor`, `language`, `greeting_name`, `role` → không đủ dữ liệu để cabin hành động.
- Không có audit log → không truy vết, không đo FAR/match-rate thực tế.
- Pipeline stateless: mỗi frame match độc lập → flip-flop, không có khái niệm "một lần tương tác".

`db/schema.sql` đã thiết kế đủ (residents, embeddings, cabins, match_events) nhưng **chưa có code C++ nào dùng**. Đây là nút thắt chặn Đề xuất 2 (elevator action) và 3 (UI/TTS).

## 2. Phạm vi

**Trong scope:**
1. Wrapper C++ cho SQLite (WAL) theo `db/schema.sql`.
2. Matching đa embedding (nhiều vector/người, lấy max similarity).
3. Tool migrate `.fdb` → SQLite.
4. State machine tương tác trong `main.cpp` + cooldown chống flip-flop.
5. Ghi `match_events` mỗi lần tương tác kết thúc.

6. **Chuyển toàn hệ thống sang SQLite làm nguồn dữ liệu duy nhất** (Giai đoạn 2): `enroll_faces` + `add_person` ghi thẳng SQLite; `.fdb` chỉ còn là input một chiều cho `migrate_fdb`.

**Ngoài scope (Đề xuất 2 & 3):** điều khiển tầng/GPIO, self-supervised capture, UI SDL2, TTS Piper, REST API. Tool `bulk_enroll` (CSV) để spec riêng — nhưng `enroll_faces` sẽ đặt nền (ghi SQLite) cho nó.

## 3. Yêu cầu chức năng

### R1 — Wrapper SQLite (`ResidentDB`)
Là hệ thống, tôi cần API C++ để đọc cư dân + embedding và ghi sự kiện.

1. KHI khởi tạo với đường dẫn DB, HỆ THỐNG PHẢI mở SQLite chế độ WAL và bật `foreign_keys=ON`.
2. NẾU file DB chưa tồn tại, HỆ THỐNG PHẢI áp dụng `db/schema.sql` để tạo bảng.
3. HỆ THỐNG PHẢI đọc toàn bộ residents `active=1` cùng danh sách embeddings vào bộ nhớ.
4. HỆ THỐNG PHẢI ghi 1 hàng `match_events` (cabin_id, resident_id nullable, similarity, action, floor_selected, latency_ms).
5. HỆ THỐNG PHẢI cập nhật `last_seen_at` và tăng `match_count` khi match thành công.
6. KHI câu lệnh SQLite lỗi, HỆ THỐNG PHẢI trả mã lỗi rõ ràng và không crash.

### R2 — Multi-embedding matching
Là hệ thống nhận diện, tôi cần so khớp với nhiều embedding/người và lấy điểm cao nhất.

1. HỆ THỐNG PHẢI nạp nhiều embedding cho mỗi resident, KHÔNG gộp trung bình.
2. KHI match query, HỆ THỐNG PHẢI tính cosine với TẤT CẢ embedding và gán resident theo **similarity lớn nhất**.
3. NẾU max similarity < ngưỡng, HỆ THỐNG PHẢI trả "unknown".
4. HỆ THỐNG PHẢI giữ hiệu năng với ~1000 người × ≤10 embedding (≤10000 vector 512-D) không làm sụt FPS đáng kể.
5. Kết quả match PHẢI trả `resident_id` + điểm similarity.

### R3 — Tool migrate `.fdb` → SQLite (`migrate_fdb`)
Là dev, tôi cần chuyển `.fdb` sang SQLite không mất dữ liệu.

1. KHI chạy `--fdb <path> --db <sqlite>`, HỆ THỐNG PHẢI đọc từng identity trong `.fdb`.
2. Với mỗi identity, HỆ THỐNG PHẢI tạo 1 hàng `residents` (name) + 1 hàng `embeddings` (source='id_photo').
3. NẾU resident cùng tên đã tồn tại, mặc định BỎ QUA + log; cờ `--overwrite` để thay thế.
4. KẾT THÚC, HỆ THỐNG PHẢI in số identity đã import và số bỏ qua.

### R4 — State machine tương tác + cooldown
Là cabin, tôi cần coi mỗi người là một "phiên tương tác" ổn định, không nhấp nháy mỗi frame.

1. HỆ THỐNG PHẢI theo dõi trạng thái phiên: `IDLE` (không có mặt) → `DETECTING` (có mặt chưa match) → `MATCHED` (match ≥ ngưỡng ổn định) → `CONFIRMED` (giữ match qua N frame / T giây).
2. KHI một resident đã CONFIRMED, HỆ THỐNG PHẢI áp cooldown (mặc định 3s) trước khi cho phép trigger lại cùng người → chống flip-flop.
3. KHI phiên chuyển sang CONFIRMED, HỆ THỐNG PHẢI ghi đúng 1 `match_events` với action='matched'.
4. NẾU có mặt nhưng không match trong T giây, HỆ THỐNG PHẢI ghi `match_events` action='unknown'.
5. State machine PHẢI dùng được cả pipeline SCRFD-only và pipeline có tracker (map theo track_id nếu có).

### R5 — `main.cpp` dùng SQLite làm nguồn dữ liệu (Giai đoạn 1)
1. HỆ THỐNG PHẢI thêm cờ CLI `--resident-db <path>` trỏ tới file SQLite.
2. KHI có `--resident-db`, đường match PHẢI đi qua SQLite (MatchEngine); các phần detect/align/tracker giữ nguyên.
3. `cabin_id` PHẢI cấu hình được qua CLI (`--cabin-id`, mặc định 1).
4. Đường `.fdb` cũ (`--face-db`) được GIỮ LẠI như chế độ test/dev nhận diện đơn giản (không tầng/ngôn ngữ), KHÔNG dùng cho vận hành cabin. Tài liệu PHẢI ghi rõ điều này.

### R6 — Chuyển `enroll_faces` + `add_person` sang SQLite (Giai đoạn 2)
Là người quản trị, tôi cần thêm/sửa cư dân trực tiếp vào SQLite mà không qua `.fdb`.

1. `enroll_faces` PHẢI ghi mỗi ảnh thành 1 hàng `embeddings` (source='id_photo'), KHÔNG gộp trung bình; tạo `residents` với `home_floor = 0` (sentinel §6) nếu chưa có.
2. `add_person` PHẢI INSERT/UPDATE resident + embeddings trong SQLite; `--merge` = thêm embedding mới, `--replace` = xóa embedding cũ của resident rồi thêm mới.
3. SAU Giai đoạn 2, `.fdb` CHỈ còn là input cho `migrate_fdb`; không tool nào khác đọc/ghi `.fdb` cho vận hành.
4. `capture_person` giữ nguyên (chỉ chụp ảnh ra folder, không đụng DB).

## 4. Yêu cầu phi chức năng

- **Hiệu năng**: match ≤ ~2ms/frame với 10000 vector (tuyến tính cosine, chấp nhận được cho v1).
- **Build**: thêm `libsqlite3-dev`; Makefile compile trên Orange Pi (arm64). Wrapper DB nên tách để test trên máy dev không cần NPU.
- **An toàn dữ liệu**: WAL; ghi `match_events` không chặn (không làm sụt FPS luồng chính).
- **Không hồi quy**: các binary `enroll_faces`, `capture_person`, `add_person` giữ nguyên hành vi.

## 5. Ngoài phạm vi / giả định

- Giả định `db/schema.sql` là nguồn chân lý schema; nếu cần cột mới sẽ dùng migration ở version sau.
- `snapshot` BLOB trong `match_events` để trống ở v1 (ảnh crop để dành Đề xuất 2/3).
- `quality_score` embedding chưa tính ở v1.

## 6. Quy ước `home_floor = 0` (sentinel "chưa đăng ký tầng")

Schema giữ nguyên `home_floor INTEGER NOT NULL` (không đổi schema). Quy ước:

- **`home_floor = 0`** nghĩa là resident **chưa được đăng ký tầng**. Không phải tầng hợp lệ (schema `floors_min` mặc định 1), nhất quán với `match_events.floor_selected` (`0 = none/cancelled`).
- Bất kỳ giá trị `home_floor <= 0` PHẢI được hệ thống hiểu là "chưa đăng ký tầng".
- Khi `migrate_fdb` import từ `.fdb` (không có thông tin tầng), HỆ THỐNG PHẢI đặt `home_floor = 0` cho mọi resident và in cảnh báo tổng số resident chưa có tầng để HR cập nhật sau.
- Ở lớp cabin (Đề xuất 2, ngoài scope spec này): khi match resident có `home_floor <= 0`, cabin PHẢI **vẫn chào tên** nhưng **KHÔNG auto-gọi tầng** — chuyển sang chọn tầng thủ công. Spec này chỉ đảm bảo dữ liệu mang đúng sentinel; hành vi cabin để spec sau.

## 7. Nguồn dữ liệu sau migrate (nguồn chân lý)

- **SQLite là nguồn dữ liệu DUY NHẤT cho vận hành cabin.** Mọi nhận diện, chào, gọi tầng, audit đều đọc/ghi SQLite.
- **`.fdb` chỉ còn 2 vai trò:** (a) input một chiều cho `migrate_fdb`; (b) chế độ test/dev nhận diện đơn giản qua `--face-db` (không có tầng/ngôn ngữ/audit) — KHÔNG dùng cho cabin thật.
- Sau Giai đoạn 2, quy trình thêm người mới đi thẳng vào SQLite (`enroll_faces`/`add_person` ghi SQLite), không còn vòng qua `.fdb`.
- `FaceDB::load()` được giữ (cho `migrate_fdb` đọc `.fdb` cũ). `FaceDB::save()` không còn dùng ở tool vận hành.

## 8. Kế hoạch 2 giai đoạn

- **Giai đoạn 1 (lõi):** `ResidentDB` + `MatchEngine` + `InteractionManager` + `migrate_fdb` + khâu nối `main.cpp` dùng SQLite. Đây là đường vận hành chính, test được ngay trên máy dev.
- **Giai đoạn 2 (dọn dẹp enroll):** chuyển `enroll_faces` + `add_person` sang ghi SQLite, gỡ `.fdb` khỏi khâu enroll. Làm sau khi Giai đoạn 1 chạy ổn.
