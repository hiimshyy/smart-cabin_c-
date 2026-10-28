# Tasks

- [x] Viết `src/occupancy_config.h/.cpp` + `tests/test_occupancy_config.cpp`; validate chặt JSON và contract v1 đúng một zone.
- [x] Viết `src/occupancy_state.h/.cpp` + `tests/test_occupancy_state.cpp`; atomic persistence, bắt buộc fsync directory, same-target failure injection và worker bounded/throttled/flush.
- [x] Viết `src/occupancy_counter.h/.cpp` + `tests/test_occupancy_counter.cpp`; entry/exit, uncertainty, ghost ID churn, unique handoff giữ certainty, chặn event cho one-to-many lineage mơ hồ, track bắt đầu giữa crossing và reconcile callback.
- [x] Thêm CLI `--occupancy-config` / `--occupancy-state`, validation và default `<config>.state` trong `app_config`.
- [x] Thêm occupancy gated trong `system_metrics` với atomic snapshot nhất quán cho count/confidence.
- [x] Thêm `draw_counting_zones` và HUD gated trong `overlay`.
- [x] Tích hợp `main.cpp`: khởi tạo sau tracker/config, update tại frame boundary, xử lý no-frame/outage, HUD/metrics và shutdown flush bất đồng bộ.
- [x] Cập nhật `Makefile`, `env.sh`, `tests/run_tests.sh`; `env.sh` truyền đúng occupancy argv theo `FACE_CABIN_ID`.
- [x] Thêm regression tests cho CLI/env, config/state/counter (gồm one-to-many churn, crossing của lineage mơ hồ, zone-first → inside, track không có occupancy slot không thể phát Exit/decrement, candidate qua sticky projection band, crossing cắt polygon khi live stream bỏ frame zone và tentative first-observation cho người đi nhanh), no-frame boundary, greedy skipped/missed observation, overlay/HUD và system metrics gating/concurrency.
- [x] Chạy targeted tests, full `bash tests/run_tests.sh`, `make -j4`, format check và targeted clang-tidy trên implementation đã đổi.
- [x] Replay NPU bằng hai recording camera thật: video một chu kỳ đạt `0 → 1 → 0`; video fast-entry/lưỡng lự đạt `0 → 1 → 0 → 1 → 0` sau sửa sticky-band.
- [ ] Live smoke test lại trên camera sau bản sửa sticky-band cuối: một người vào/ra nhanh, lưỡng lự cùng phía và hai chu kỳ liên tiếp không drift.
- [ ] Stress test nhiều người: đi sát nhau, ngược chiều đồng thời, che khuất và nhập/tách bbox.
- [ ] Soak test service, restart persistence và nhiệt độ; live trước đó từng đạt 86.4°C.
- [ ] Quyết định/expose cơ chế operator gọi `reconcile(count)` hoặc `reconcile_empty()` khi state `Uncertain`.

Chi tiết trạng thái, bằng chứng và lệnh tiếp tục xem tại `handoff.md`.
