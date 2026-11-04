# Implementation Plan: Điều phối inference theo thanh ghi cửa 8472-2 (door-register-inference)

## Overview

Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

Kế hoạch triển khai việc dùng thanh ghi cửa `8472-2` (slave 2) từ `CONN_DATA` của iot_core để quyết định frame nào chạy YOLO person và SCRFD face. Khi cửa đóng, cabin hết person track và qua empty grace, cả hai stage ngừng chạy; model vẫn load một lần lúc startup.

Thứ tự đi từ phần độc lập trước: subscription/parser Socket.IO và `DoorInferenceScheduler` thuần (task 1–2), sau đó tích hợp frame loop trong `main.cpp` (task 3), log `[scheduler]` (task 4), build và unit tests (task 5), rồi kiểm chứng trên dữ liệu và phần cứng thật (task 6–7).

Task 1–6 đã xong. Task 7 còn mở.

## Tasks

- [x] 1. Subscribe/parse `CONN_DATA` và lọc đúng slave 2.
- [x] 2. Scheduler độc lập NPU/Socket.IO với unit tests.
- [x] 3. Tích hợp main, skip preprocessing và NPU khi idle.
- [x] 4. Log state và số lần chạy model theo window hiện có.
- [x] 5. Build và unit tests.
- [x] 6. Đọc thử `CONN_SUBSCRIBE`/`CONN_DATA` thật ở chế độ chỉ đọc; xác nhận `8472-2` có trong snapshot và partial update.
- [ ] 7. Smoke test board/camera thật; đo độ trễ mở cửa tới YOLO và NPU idle.

## Notes

- Task Dependency Graph dựa trên luồng dữ liệu trong `design.md`: frame loop trong `main.cpp` (3) đọc atomic state do door callback ghi từ `SocketIoClient` (1) và gọi `DoorInferenceScheduler` (2); log `[scheduler]` (4) in state và số lần chạy từ frame loop đã tích hợp (3); build và unit tests (5) cần task 1–4.
- Task 6 kiểm tra trên dữ liệu thật giả định của parser ở task 1 (`8472-2` có trong snapshot và trong partial update), nên đặt sau task 1.
- Theo requirements.md, cần smoke test board/camera thật (7) trước khi deploy. Task 7 cần build đã qua test (5) và định dạng dữ liệu đã xác nhận (6). Theo mục Testing Strategy của `design.md`, task này gồm đo độ trễ từ lúc mở cửa tới lần YOLO đầu tiên và xác nhận NPU idle qua window log `yolo_runs=0 scrfd_runs=0` ở state `IDLE`.
- Các thông số mặc định (nhịp YOLO/SCRFD, empty grace 3000 ms, TTL dữ liệu cửa 5000 ms) cần đo và chỉnh trong lúc smoke test; đổi số cần build lại.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1", "2"] },
    { "id": 1, "tasks": ["3", "6"] },
    { "id": 2, "tasks": ["4"] },
    { "id": 3, "tasks": ["5"] },
    { "id": 4, "tasks": ["7"] }
  ]
}
```
