# Implementation Plan: Đếm người trong cabin bằng Counting Zone (cabin-occupancy)

## Overview

Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)
Bàn giao: [handoff.md](handoff.md)

Kế hoạch triển khai bộ đếm người trong cabin theo sự kiện: person track đi qua counting zone ở cửa thang máy mới làm count tăng hoặc giảm, count được lưu bất đồng bộ và hiển thị qua HUD/metrics khi tính năng bật.

Thứ tự đi từ module thuần trước: config, state persistence và counter có unit test (task 1–3), sau đó CLI, metrics, overlay (task 4–6), tích hợp `main.cpp` (task 7), build/env/test script (task 8), regression tests và kiểm tra build (task 9–10), replay NPU (task 11), rồi kiểm chứng live trên camera (task 12–14) và cơ chế reconcile cho operator (task 15).

Task 1–11 đã xong. Task 12–15 còn mở.

## Tasks

- [x] 1. Viết `src/occupancy_config.h/.cpp` + `tests/test_occupancy_config.cpp`; validate chặt JSON và contract v1 đúng một zone.
- [x] 2. Viết `src/occupancy_state.h/.cpp` + `tests/test_occupancy_state.cpp`; atomic persistence, bắt buộc fsync directory, same-target failure injection và worker bounded/throttled/flush.
- [x] 3. Viết `src/occupancy_counter.h/.cpp` + `tests/test_occupancy_counter.cpp`; entry/exit, uncertainty, ghost ID churn, unique handoff giữ certainty, chặn event cho one-to-many lineage mơ hồ, track bắt đầu giữa crossing và reconcile callback.
- [x] 4. Thêm CLI `--occupancy-config` / `--occupancy-state`, validation và default `<config>.state` trong `app_config`.
- [x] 5. Thêm occupancy gated trong `system_metrics` với atomic snapshot nhất quán cho count/confidence.
- [x] 6. Thêm `draw_counting_zones` và HUD gated trong `overlay`.
- [x] 7. Tích hợp `main.cpp`: khởi tạo sau tracker/config, update tại frame boundary, xử lý no-frame/outage, HUD/metrics và shutdown flush bất đồng bộ.
- [x] 8. Cập nhật `Makefile`, `env.sh`, `tests/run_tests.sh`; `env.sh` truyền đúng occupancy argv theo `FACE_CABIN_ID`.
- [x] 9. Thêm regression tests cho CLI/env, config/state/counter (gồm one-to-many churn, crossing của lineage mơ hồ, zone-first → inside, track không có occupancy slot không thể phát Exit/decrement, candidate qua sticky projection band, crossing cắt polygon khi live stream bỏ frame zone và tentative first-observation cho người đi nhanh), no-frame boundary, greedy skipped/missed observation, overlay/HUD và system metrics gating/concurrency.
- [x] 10. Chạy targeted tests, full `bash tests/run_tests.sh`, `make -j4`, format check và targeted clang-tidy trên implementation đã đổi.
- [x] 11. Replay NPU bằng hai recording camera thật: video một chu kỳ đạt `0 → 1 → 0`; video fast-entry/lưỡng lự đạt `0 → 1 → 0 → 1 → 0` sau sửa sticky-band.
- [ ] 12. Live smoke test lại trên camera sau bản sửa sticky-band cuối: một người vào/ra nhanh, lưỡng lự cùng phía và hai chu kỳ liên tiếp không drift.
- [ ] 13. Stress test nhiều người: đi sát nhau, ngược chiều đồng thời, che khuất và nhập/tách bbox.
- [ ] 14. Soak test service, restart persistence và nhiệt độ; live trước đó từng đạt 86.4°C.
- [ ] 15. Quyết định/expose cơ chế operator gọi `reconcile(count)` hoặc `reconcile_empty()` khi state `Uncertain`.

## Notes

- Chi tiết trạng thái, bằng chứng và lệnh tiếp tục xem tại `handoff.md`.
- Task Dependency Graph dựa trên luồng dữ liệu trong `design.md`: `OccupancyCounter` (3) nhận `OccupancyConfig` (1) và `OccupancyState` (2); `draw_counting_zones` (6) vẽ zone từ `OccupancyConfig` (1); `main.cpp` (7) nối counter, CLI, metrics và overlay (3–6); `env.sh` (8) truyền các option CLI của task 4 và `Makefile` cần `main.cpp` đã tích hợp; regression test CLI/env và no-frame boundary (9) cần task 7 và 8.
- Task 12–14 chạy tuần tự trên cùng camera và NPU của cabin 2. Không chạy service và app thủ công đồng thời (xem `handoff.md`). Theo `handoff.md`, chỉ thử nhiều người (13) sau khi live smoke một người (12) đạt, và cần bảo đảm tản nhiệt trước soak test (14).
- Task 15 đặt sau soak test vì `handoff.md` ghi rằng cần đánh giá vận hành thực tế (tần suất state thành `Uncertain`) trước khi chốt có cần cơ chế reconcile bên ngoài hay không. API `reconcile(count)`/`reconcile_empty()` đã có từ task 3.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1", "2", "4", "5"] },
    { "id": 1, "tasks": ["3", "6"] },
    { "id": 2, "tasks": ["7"] },
    { "id": 3, "tasks": ["8"] },
    { "id": 4, "tasks": ["9"] },
    { "id": 5, "tasks": ["10"] },
    { "id": 6, "tasks": ["11"] },
    { "id": 7, "tasks": ["12"] },
    { "id": 8, "tasks": ["13"] },
    { "id": 9, "tasks": ["14"] },
    { "id": 10, "tasks": ["15"] }
  ]
}
```
