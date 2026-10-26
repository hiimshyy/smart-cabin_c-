# Tasks

- [x] Subscribe/parse `CONN_DATA` và lọc đúng slave 2.
- [x] Scheduler độc lập NPU/Socket.IO với unit tests.
- [x] Tích hợp main, skip preprocessing và NPU khi idle.
- [x] Log state và số lần chạy model theo window hiện có.
- [x] Build và unit tests.
- [x] Đọc thử `CONN_SUBSCRIBE`/`CONN_DATA` thật ở chế độ chỉ đọc; xác nhận `8472-2` có trong snapshot và partial update.
- [ ] Smoke test board/camera thật; đo độ trễ mở cửa tới YOLO và NPU idle.
