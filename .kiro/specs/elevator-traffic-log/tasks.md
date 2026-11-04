# Implementation Plan: Nhật ký di chuyển thang máy (elevator-traffic-log)

## Overview

Kế hoạch triển khai tập dữ liệu traffic cho thang 2 (`cabin_id=2`, slave `-2`, camera `cam2`): điểm dừng, chuyến cabin, chuyến hành khách, gọi tầng trong cabin và gọi tầng ngoài, lưu cục bộ vào `db/traffic.db`. Yêu cầu ở `requirements.md`, thiết kế ở `design.md`.

Thứ tự đi từ dưới lên: parse thanh ghi và timeline (giai đoạn 1), lineage của occupancy (giai đoạn 2), ghép chuyến (giai đoạn 3), lưu trữ (giai đoạn 4), rồi mới tích hợp vào `main.cpp` (giai đoạn 5) và viết công cụ quản trị (giai đoạn 6). Các state machine ở giai đoạn 1–3 nhận timestamp từ bên ngoài, nên test được bằng replay fixture `CONN_DATA` mà không cần thread hay mạng. Giai đoạn 7 là hiệu chỉnh, kiểm chứng trên thiết bị và tài liệu vận hành.

## Tasks

- [ ] 0. Xác nhận đầu vào
  - [x] 0.1 Trả lời câu hỏi mở 1–5 (xem "Quyết định đã chốt" trong `requirements.md`).
  - [x] 0.2 Ghi mẫu `CONN_DATA` 10 phút bằng client chỉ đọc (03/11/2026, 14:49–14:59). Đã xác nhận định dạng, chu kỳ gửi (~5,6 s/slave, chỉ gửi giá trị đổi), cách quy đổi tầng (`8465 − 1` = bit `n`), gọi ngoài dùng chung cho nhóm thang, và phát hiện chuyến chạy khớp `8448`. Script và dữ liệu ở `.agents/tasks/elevator-traffic-log/`.
  - [ ] 0.3 (Giai đoạn 0b) Ghi mẫu dài hơn (gợi ý 2 giờ cao điểm buổi sáng), cũng chỉ đọc, để: quan sát ít nhất một lệnh gọi tự động `8193-2` làm bật bit `8211` tương ứng; xem có `8465 = 1` (tầng B) và `8465 = 10` (tầng 9); đo tỉ lệ Entry/Exit của bộ đếm so với số lệnh gọi trong cabin.
  - [ ] 0.4 Chép bản ghi mẫu 10 phút thành fixture `tests/fixtures/conn_data_20261103.jsonl` (chỉ giá trị thanh ghi và device code).

- [ ] 1. Thanh ghi, timeline, lệnh gọi
  - [ ] 1.1 Viết `src/elevator_registers.h/.cpp` + `tests/test_elevator_registers.cpp`: parser nhiều thanh ghi và nhiều slave, quy đổi tầng, decode bit, từ chối giá trị sai.
  - [ ] 1.2 Sửa `socketio_client`: `RegisterCallback` mang session, subscribe khi bật door hoặc traffic, giữ nguyên `DoorCallback` và test hiện có.
  - [ ] 1.3 Viết `src/elevator_state.h/.cpp` + `tests/test_elevator_state.cpp`: `ElevatorStateTracker`, `RegisterInbox`, `CabinTimeline` cho từng slave; quy tắc gap (session, inbox tràn, `run_timeout`); so khớp `8448`; replay fixture.
  - [ ] 1.4 Viết `src/call_tracker.h/.cpp` + `tests/test_call_tracker.cpp`: gọi trong cabin (R14), gọi ngoài (R13) có phân bổ cho thang 1, xác nhận lệnh gọi tự động; replay fixture.

- [ ] 2. Lineage của occupancy
  - [ ] 2.1 Thêm `occupant_id` vào `OccupancyEvent`, `TrackState`, `Ghost`; thêm `track_occupants` vào `OccupancySnapshot`. Bổ sung test lineage. Toàn bộ `test_occupancy_counter` hiện có phải pass mà không sửa assertion.

- [ ] 3. Ghép chuyến
  - [ ] 3.1 Viết `src/trip_assembler.h/.cpp` + `tests/test_trip_assembler.cpp`: gán tầng có dung sai 6 s, gắn gọi ngoài/gọi trong cabin, danh tính trước Entry, `identity_conflict`, `load_implausible`, timeout/reconcile/shutdown, giới hạn bộ nhớ.

- [ ] 4. Lưu trữ
  - [ ] 4.1 Viết `db/traffic_schema.sql`, `src/traffic_db.h/.cpp` + `tests/test_traffic_db.cpp`: schema (gồm `car_calls`, `hall_calls`), insert theo lô, purge, export CSV có nhãn tầng, thao tác admin và audit.
  - [ ] 4.2 Viết `src/visitor_gallery.h/.cpp` + `tests/test_visitor_gallery.cpp`.
  - [ ] 4.3 Viết `src/traffic_log_writer.h/.cpp` + `tests/test_traffic_log_writer.cpp`: queue drop-oldest, disk guard, JPEG `0600`, flush khi tắt.

- [ ] 5. Tích hợp
  - [ ] 5.1 Thêm tham số CLI vào `app_config` và `env.sh`; cập nhật `test_app_config` và `test_env_face_cabin.sh`.
  - [ ] 5.2 Tích hợp `main.cpp`: thứ tự frame boundary như trong design, nhánh không có frame, `match(emb, -1)` để lấy resident gần nhất, inbox cho floor-call decision, metrics, shutdown.
  - [ ] 5.3 Cập nhật `Makefile` (binary `traffic_admin`), `tests/run_tests.sh`, `.gitignore` (nếu thêm đường dẫn mới).

- [ ] 6. Công cụ quản trị
  - [ ] 6.1 Viết `src/traffic_admin.cpp`: `visitors list|show`, `merge`, `delete`, `promote` (resident mới hoặc có sẵn), `suggest`, `export`. Có xác nhận và audit. Test các lệnh ghi trên DB tạm. Người dùng là quản lý hoặc dev, nên mỗi lệnh có `--help` và ví dụ.

- [ ] 7. Kiểm chứng
  - [ ] 7.1 Hiệu chỉnh `visitor_match_thr` trên các recording đã có: đo tỉ lệ gộp nhầm và tách nhầm, chốt giá trị mặc định.
  - [ ] 7.2 Chạy `bash tests/run_tests.sh`, `make -j4`, `make format-check` và clang-tidy cho các file đã sửa.
  - [ ] 7.3 Live test 1 ngày: đối chiếu số chuyến cabin với chênh lệch `8448`/`8449`; kiểm tra tầng vào/ra thủ công trên ít nhất 20 chuyến hành khách; kiểm tra thời gian chờ trên ít nhất 10 lệnh gọi ngoài.
  - [ ] 7.4 Viết hướng dẫn vận hành trong `README.md`: bật/tắt tính năng, export, sao lưu `traffic.db`, xác minh danh tính, checklist pháp lý trước khi bật nhận lại visitor và lưu ảnh.

## Notes

- Task 0.3 là "giai đoạn 0b" được nhắc trong `requirements.md` (kiểm chứng lệnh gọi tự động bật bit `8211`).
- Task 0.3 chỉ ghi dữ liệu để xác nhận giả định, không chặn các task code.
- Task Dependency Graph dựa trên luồng dữ liệu trong `design.md`: `CabinTimeline` cần `RegisterUpdate` (1.1) và fixture (0.4); `CallTracker` cần sự kiện timeline (1.3); `TripAssembler` cần timeline, lệnh gọi và lineage (1.3, 1.4, 2.1); `TrafficDB` và `VisitorGallery` lưu và xử lý bản ghi của assembler; `TrafficLogWriter` dùng cả hai; `main.cpp` nối mọi module; `Makefile` cần cả `main.cpp` lẫn `traffic_admin.cpp`.

### Ngoài phạm vi spec này (đề xuất mở việc riêng)

- `door-register-inference`: quy tắc freshness 5 s cho `8472-2` không phù hợp với `CONN_DATA` chỉ gửi giá trị đổi; ngày 03/11 có 745 lần `DEGRADED door_fresh=0`. Cần điều tra xem việc này có làm bộ đếm người bỏ sót lượt vào/ra không.
- `cabin-occupancy`: count production đang ở 275 (`Uncertain`); cần cơ chế reconcile (task còn mở trong spec đó).
- Gọi tầng tự động: 15/16 lệnh ngày 02/11 và 18/18 lệnh ngày 03/11 có ACK bị đánh dấu `MALFORMED_ACK`. Cần kiểm tra định dạng ACK của server so với parser.
- Tầng B: `floor_to_register_value` từ chối tầng 0, nên hiện không gọi tự động được tầng B.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["0.1", "0.2"] },
    { "id": 1, "tasks": ["0.3", "0.4", "1.1", "2.1", "5.1"] },
    { "id": 2, "tasks": ["1.2", "1.3"] },
    { "id": 3, "tasks": ["1.4"] },
    { "id": 4, "tasks": ["3.1"] },
    { "id": 5, "tasks": ["4.1", "4.2"] },
    { "id": 6, "tasks": ["4.3", "6.1"] },
    { "id": 7, "tasks": ["5.2"] },
    { "id": 8, "tasks": ["5.3"] },
    { "id": 9, "tasks": ["7.1"] },
    { "id": 10, "tasks": ["7.2", "7.4"] },
    { "id": 11, "tasks": ["7.3"] }
  ]
}
```
