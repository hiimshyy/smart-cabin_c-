# Implementation Plan: Adaptive inference theo trạng thái thang máy (adaptive-inference)

## Overview

Trạng thái: **Chưa triển khai**
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

Kế hoạch giảm số lần chạy YOLO person, SCRFD và face recognition trên NPU khi cabin không cần nhận diện, dựa trên trạng thái thang máy số 2 từ IoT Core, person track và identity của từng track. Model vẫn load một lần lúc startup; adaptive inference chỉ bỏ qua lần chạy trong frame.

Thứ tự đi từ hợp đồng và số đo trước, code sau: chốt hợp đồng IoT Core và đo baseline (task 0–1), viết logic thuần có unit test (task 2–4), nối state event qua Socket.IO (task 5), rồi mới tích hợp main loop (task 6), floor-call policy (task 7), cấu hình và quan sát (task 8), verification (task 9) và rollout từng bước (task 10).

## Tasks

- [ ] 0. Chốt hợp đồng IoT Core
  - [ ] 0.1 Chốt tên Socket.IO event state và payload/envelope thực tế.
  - [ ] 0.2 Lập mapping Modbus → `doorState`, `serviceMode`, `moving`, `direction`, `callPending`.
  - [ ] 0.3 Chốt cách request/receive snapshot ngay sau connect và reconnect.
  - [ ] 0.4 Chốt `sequence`, `bootId/sessionId` và hành vi khi IoT Core restart.
  - [ ] 0.5 Chốt heartbeat/state refresh interval và chọn `state_ttl_ms`.
  - [ ] 0.6 Chốt policy khi state stale: đề xuất vẫn recognition nhưng suppress floor command.

- [ ] 1. Baseline measurement
  - [ ] 1.1 Thêm counter số lần YOLO, SCRFD và recognition thực sự chạy.
  - [ ] 1.2 Ghi stage latency và tổng `awnn_run` khi cabin trống/có người.
  - [ ] 1.3 Thu CPU, RAM, nhiệt độ và FPS trong ít nhất 30 phút idle.
  - [ ] 1.4 Tạo video fixture: empty, door open không người, một người, nhiều người, track split.
  - [ ] 1.5 Lưu baseline missed-person, recognition latency và NPU executions để so rollout.

- [ ] 2. ElevatorStateProvider
  - [ ] 2.1 Tạo `src/elevator_state.h/.cpp` với enum và immutable snapshot.
  - [ ] 2.2 Parse state event có giới hạn kích thước và validation field.
  - [ ] 2.3 Filter đúng device code và elevator số 2.
  - [ ] 2.4 Bỏ event sequence cũ; hỗ trợ boot/session reset theo task 0.4.
  - [ ] 2.5 Dùng monotonic receive time để tính TTL.
  - [ ] 2.6 Thêm `on_disconnected()` và snapshot stale/unknown.
  - [ ] 2.7 Unit test malformed payload, wrong elevator, ordering, reconnect và TTL.

- [ ] 3. InferenceScheduler thuần logic
  - [ ] 3.1 Tạo `src/inference_scheduler.h/.cpp`, không phụ thuộc OpenCV/NPU/socket/DB.
  - [ ] 3.2 Implement state `IDLE`, `ARMED`, `DOOR_ACTIVE`, `OCCUPIED_UNRESOLVED`, `OCCUPIED_RESOLVED`, `EMPTY_GRACE`, `DEGRADED`, `INHIBITED`.
  - [ ] 3.3 Dùng monotonic deadlines cho person/face cadence.
  - [ ] 3.4 Door opening làm person scan due ngay.
  - [ ] 3.5 Implement debounce cửa, armed timeout và empty grace.
  - [ ] 3.6 Implement baseline mode và shadow mode.
  - [ ] 3.7 Tạo deterministic transition reason và counters theo state.
  - [ ] 3.8 Unit test transition table và variable camera FPS.

- [ ] 4. Track summary và per-track policy
  - [ ] 4.1 Tạo `TrackSummary` từ `active_tracks()`.
  - [ ] 4.2 Chỉ `Tracked && !predicted_only` được kích hoạt SCRFD.
  - [ ] 4.3 Known latch được đánh dấu resolved; unknown retry vẫn unresolved đến deadline.
  - [ ] 4.4 Track mới trong resolved state kích hoạt lại face/recognition.
  - [ ] 4.5 Nhiều track: known track không làm dừng recognition của unknown track.
  - [ ] 4.6 Unit test known/unknown/new/lost/predicted-only và track split.

- [ ] 5. Tích hợp Socket.IO state event
  - [ ] 5.1 Refactor client thành `IoTCoreClient` hoặc mở rộng `SocketIoClient` có state callback.
  - [ ] 5.2 Phân luồng command ACK và server event trên cùng connection.
  - [ ] 5.3 Gửi subscribe/request snapshot sau namespace ready nếu protocol yêu cầu.
  - [ ] 5.4 Callback state sau khi nhả mutex; không block command ACK.
  - [ ] 5.5 Reconnect phải nhận snapshot trước khi state được coi là fresh.
  - [ ] 5.6 Fake server test event/ACK/ping xen kẽ, malformed event và reconnect.
  - [ ] 5.7 Bảo đảm shutdown worker interruptible, không chờ connect/read timeout dài.

- [ ] 6. Tích hợp main inference loop
  - [ ] 6.1 Gọi scheduler trước YOLO bằng elevator snapshot và track summary.
  - [ ] 6.2 Thay modulo cố định bằng `run_person` khi adaptive mode bật.
  - [ ] 6.3 Re-plan sau YOLO để có thể chạy SCRFD ngay khi thấy person đầu tiên.
  - [ ] 6.4 Gate `awnn_run` và decode của SCRFD theo `run_face`.
  - [ ] 6.5 Clear `faces`, association và labels khi skip SCRFD.
  - [ ] 6.6 Recognition chỉ chạy khi `allow_recognition` và track eligible.
  - [ ] 6.7 Khi YOLO/SCRFD đều skip, bỏ preprocessing nếu không còn consumer khác.
  - [ ] 6.8 Không gọi tracker update với empty detections trên skipped YOLO frame.

- [ ] 7. Tích hợp floor-call policy
  - [ ] 7.1 Thêm inhibit input/reason vào `FloorCallCoordinator`.
  - [ ] 7.2 Maintenance/emergency/fire suppress command mới.
  - [ ] 7.3 State stale mặc định vẫn recognition nhưng suppress command.
  - [ ] 7.4 Door transition không xóa cabin visit khi còn person.
  - [ ] 7.5 Command đã sent không bị cancel/resend do policy đổi.
  - [ ] 7.6 Unit test normal/inhibited/stale và policy đổi khi command in-flight.

- [ ] 8. Cấu hình và quan sát
  - [ ] 8.1 Thêm `adaptive_inference_enabled` và `adaptive_inference_shadow`.
  - [ ] 8.2 Thêm TTL, door grace, armed timeout và unknown resolve timeout.
  - [ ] 8.3 Thêm person/face FPS theo state với validation tập trung.
  - [ ] 8.4 Log effective config không chứa secret/RTSP URL.
  - [ ] 8.5 Log state transition rate-limited và không có PII.
  - [ ] 8.6 Thêm run/skip counters và latency metrics theo state.
  - [ ] 8.7 Một config rollback đưa pipeline về baseline hiện tại mà không restart DB/migration.

- [ ] 9. Verification
  - [ ] 9.1 Unit tests của task 2–8 qua trên host.
  - [ ] 9.2 Build đầy đủ trên Orange Pi/NPU SDK.
  - [ ] 9.3 Shadow replay cùng video, so quyết định scheduler với baseline.
  - [ ] 9.4 Xác nhận latency door-opening → first YOLO ≤150 ms.
  - [ ] 9.5 Xác nhận person-first-seen → first recognition tăng không quá 200 ms so baseline.
  - [ ] 9.6 Xác nhận idle `awnn_run` giảm ít nhất 70% mà không tăng missed-person.
  - [ ] 9.7 Test IoT offline từ startup, stale state, reconnect và restart.
  - [ ] 9.8 Test nhiều người: resolved resident không chặn unresolved/new person.
  - [ ] 9.9 Test service modes bằng fake server/simulator.
  - [ ] 9.10 Soak test tối thiểu 30 phút idle và nhiều cabin visit; theo dõi nhiệt độ/RAM/FPS.

- [ ] 10. Rollout
  - [ ] 10.1 Deploy shadow mode trên thang máy số 2.
  - [ ] 10.2 Bật SCRFD idle gating, giữ YOLO baseline.
  - [ ] 10.3 Bật adaptive YOLO cadence và degraded fallback.
  - [ ] 10.4 Bật per-track face/recognition gating.
  - [ ] 10.5 Bật service-mode inhibit cho floor command sau khi task 0 hoàn tất.
  - [ ] 10.6 Theo dõi metrics, false negative và rollback drill.

## Notes

### Thứ tự triển khai đề xuất

1. Hoàn tất task 0 và đo baseline task 1.
2. Làm task 2/3/4 với unit tests, chưa chạm pipeline thật.
3. Tích hợp state event task 5 và chạy shadow mode.
4. Tích hợp inference task 6 theo từng stage: SCRFD trước, YOLO sau.
5. Nối floor policy task 7 sau khi state contract ổn định.
6. Hoàn thiện config/metrics task 8, verification task 9 và rollout task 10.

### Về Task Dependency Graph

- Graph chỉ ghi phụ thuộc bắt buộc, dựa trên luồng dữ liệu trong `design.md`: `ElevatorStateProvider` (2.x) cần payload, mapping và quy tắc `sequence`/`bootId` (0.1, 0.2, 0.4); `InferenceScheduler` (3.x) nhận `ElevatorStateSnapshot` (2.1) và dùng policy stale (0.6); `TrackSummary` (4.x) là input của scheduler; main loop (6.1) cần provider, scheduler, track summary, state event qua Socket.IO (5.x) và cờ adaptive/shadow (8.1).
- Baseline (1.5) chỉ chặn task 6.x, vì phải đo baseline trước khi đổi pipeline. Task 2–4 không chạm pipeline thật nên graph cho phép làm song song với task 1.
- Task 8.1 đứng trước 6.1 vì shadow mode (bước 3 ở trên) cần cờ `adaptive_inference_shadow`. Các task 8.x còn lại theo sau.
- Trong task 6, SCRFD gating (6.3–6.5) đi trước YOLO cadence (6.2, 6.8). Task 6.7 là tối ưu preprocessing, làm sau khi gating đã ổn định.
- Task 7.1 bắt đầu sau khi scheduler và Socket.IO đã qua test (3.8, 5.6), tức state contract đã ổn định.
- Task 10.x chạy tuần tự trên thang máy số 2, sau khi toàn bộ verification qua.
- Một số task cùng wave sửa cùng file (ví dụ 3.3, 3.5, 3.6, 3.7 cùng sửa `src/inference_scheduler.cpp`; 2.2 và 2.5 cùng sửa `src/elevator_state.cpp`). Nếu chạy song song thì phải phối hợp, hoặc làm tuần tự.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "1.1", "1.3", "1.4"] },
    { "id": 1, "tasks": ["1.2", "2.1", "5.1"] },
    { "id": 2, "tasks": ["1.5", "2.2", "2.5", "3.1", "5.2", "5.3", "5.7"] },
    { "id": 3, "tasks": ["2.3", "2.4", "2.6", "3.2", "4.1", "5.4"] },
    { "id": 4, "tasks": ["2.7", "3.3", "3.5", "3.6", "3.7", "4.2", "4.3", "4.4", "5.5"] },
    { "id": 5, "tasks": ["3.4", "4.5", "5.6", "8.1"] },
    { "id": 6, "tasks": ["3.8", "4.6", "8.2"] },
    { "id": 7, "tasks": ["6.1", "7.1", "8.3"] },
    { "id": 8, "tasks": ["6.4", "7.2", "7.3", "7.4", "7.5", "8.4", "8.5", "8.6"] },
    { "id": 9, "tasks": ["6.3", "6.5", "7.6"] },
    { "id": 10, "tasks": ["6.2", "6.6"] },
    { "id": 11, "tasks": ["6.8"] },
    { "id": 12, "tasks": ["6.7"] },
    { "id": 13, "tasks": ["8.7"] },
    { "id": 14, "tasks": ["9.1", "9.2"] },
    { "id": 15, "tasks": ["9.3", "9.4", "9.5", "9.6", "9.7", "9.8", "9.9"] },
    { "id": 16, "tasks": ["9.10"] },
    { "id": 17, "tasks": ["10.1"] },
    { "id": 18, "tasks": ["10.2"] },
    { "id": 19, "tasks": ["10.3"] },
    { "id": 20, "tasks": ["10.4"] },
    { "id": 21, "tasks": ["10.5"] },
    { "id": 22, "tasks": ["10.6"] }
  ]
}
```
