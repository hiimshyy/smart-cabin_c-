# Tasks: Adaptive inference theo trạng thái thang máy

Trạng thái: **Chưa triển khai**
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

## P0 — Chốt hợp đồng IoT Core

- [ ] P0.1 Chốt tên Socket.IO event state và payload/envelope thực tế.
- [ ] P0.2 Lập mapping Modbus → `doorState`, `serviceMode`, `moving`, `direction`, `callPending`.
- [ ] P0.3 Chốt cách request/receive snapshot ngay sau connect và reconnect.
- [ ] P0.4 Chốt `sequence`, `bootId/sessionId` và hành vi khi IoT Core restart.
- [ ] P0.5 Chốt heartbeat/state refresh interval và chọn `state_ttl_ms`.
- [ ] P0.6 Chốt policy khi state stale: đề xuất vẫn recognition nhưng suppress floor command.

## P1 — Baseline measurement

- [ ] P1.1 Thêm counter số lần YOLO, SCRFD và recognition thực sự chạy.
- [ ] P1.2 Ghi stage latency và tổng `awnn_run` khi cabin trống/có người.
- [ ] P1.3 Thu CPU, RAM, nhiệt độ và FPS trong ít nhất 30 phút idle.
- [ ] P1.4 Tạo video fixture: empty, door open không người, một người, nhiều người, track split.
- [ ] P1.5 Lưu baseline missed-person, recognition latency và NPU executions để so rollout.

## P2 — ElevatorStateProvider

- [ ] P2.1 Tạo `src/elevator_state.h/.cpp` với enum và immutable snapshot.
- [ ] P2.2 Parse state event có giới hạn kích thước và validation field.
- [ ] P2.3 Filter đúng device code và elevator số 2.
- [ ] P2.4 Bỏ event sequence cũ; hỗ trợ boot/session reset theo P0.4.
- [ ] P2.5 Dùng monotonic receive time để tính TTL.
- [ ] P2.6 Thêm `on_disconnected()` và snapshot stale/unknown.
- [ ] P2.7 Unit test malformed payload, wrong elevator, ordering, reconnect và TTL.

## P3 — InferenceScheduler thuần logic

- [ ] P3.1 Tạo `src/inference_scheduler.h/.cpp`, không phụ thuộc OpenCV/NPU/socket/DB.
- [ ] P3.2 Implement state `IDLE`, `ARMED`, `DOOR_ACTIVE`, `OCCUPIED_UNRESOLVED`,
      `OCCUPIED_RESOLVED`, `EMPTY_GRACE`, `DEGRADED`, `INHIBITED`.
- [ ] P3.3 Dùng monotonic deadlines cho person/face cadence.
- [ ] P3.4 Door opening làm person scan due ngay.
- [ ] P3.5 Implement debounce cửa, armed timeout và empty grace.
- [ ] P3.6 Implement baseline mode và shadow mode.
- [ ] P3.7 Tạo deterministic transition reason và counters theo state.
- [ ] P3.8 Unit test transition table và variable camera FPS.

## P4 — Track summary và per-track policy

- [ ] P4.1 Tạo `TrackSummary` từ `active_tracks()`.
- [ ] P4.2 Chỉ `Tracked && !predicted_only` được kích hoạt SCRFD.
- [ ] P4.3 Known latch được đánh dấu resolved; unknown retry vẫn unresolved đến deadline.
- [ ] P4.4 Track mới trong resolved state kích hoạt lại face/recognition.
- [ ] P4.5 Nhiều track: known track không làm dừng recognition của unknown track.
- [ ] P4.6 Unit test known/unknown/new/lost/predicted-only và track split.

## P5 — Tích hợp Socket.IO state event

- [ ] P5.1 Refactor client thành `IoTCoreClient` hoặc mở rộng `SocketIoClient` có state callback.
- [ ] P5.2 Phân luồng command ACK và server event trên cùng connection.
- [ ] P5.3 Gửi subscribe/request snapshot sau namespace ready nếu protocol yêu cầu.
- [ ] P5.4 Callback state sau khi nhả mutex; không block command ACK.
- [ ] P5.5 Reconnect phải nhận snapshot trước khi state được coi là fresh.
- [ ] P5.6 Fake server test event/ACK/ping xen kẽ, malformed event và reconnect.
- [ ] P5.7 Bảo đảm shutdown worker interruptible, không chờ connect/read timeout dài.

## P6 — Tích hợp main inference loop

- [ ] P6.1 Gọi scheduler trước YOLO bằng elevator snapshot và track summary.
- [ ] P6.2 Thay modulo cố định bằng `run_person` khi adaptive mode bật.
- [ ] P6.3 Re-plan sau YOLO để có thể chạy SCRFD ngay khi thấy person đầu tiên.
- [ ] P6.4 Gate `awnn_run` và decode của SCRFD theo `run_face`.
- [ ] P6.5 Clear `faces`, association và labels khi skip SCRFD.
- [ ] P6.6 Recognition chỉ chạy khi `allow_recognition` và track eligible.
- [ ] P6.7 Khi YOLO/SCRFD đều skip, bỏ preprocessing nếu không còn consumer khác.
- [ ] P6.8 Không gọi tracker update với empty detections trên skipped YOLO frame.

## P7 — Tích hợp floor-call policy

- [ ] P7.1 Thêm inhibit input/reason vào `FloorCallCoordinator`.
- [ ] P7.2 Maintenance/emergency/fire suppress command mới.
- [ ] P7.3 State stale mặc định vẫn recognition nhưng suppress command.
- [ ] P7.4 Door transition không xóa cabin visit khi còn person.
- [ ] P7.5 Command đã sent không bị cancel/resend do policy đổi.
- [ ] P7.6 Unit test normal/inhibited/stale và policy đổi khi command in-flight.

## P8 — Cấu hình và quan sát

- [ ] P8.1 Thêm `adaptive_inference_enabled` và `adaptive_inference_shadow`.
- [ ] P8.2 Thêm TTL, door grace, armed timeout và unknown resolve timeout.
- [ ] P8.3 Thêm person/face FPS theo state với validation tập trung.
- [ ] P8.4 Log effective config không chứa secret/RTSP URL.
- [ ] P8.5 Log state transition rate-limited và không có PII.
- [ ] P8.6 Thêm run/skip counters và latency metrics theo state.
- [ ] P8.7 Một config rollback đưa pipeline về baseline hiện tại mà không restart DB/migration.

## P9 — Verification

- [ ] P9.1 Unit tests P2–P8 qua trên host.
- [ ] P9.2 Build đầy đủ trên Orange Pi/NPU SDK.
- [ ] P9.3 Shadow replay cùng video, so quyết định scheduler với baseline.
- [ ] P9.4 Xác nhận latency door-opening → first YOLO ≤150 ms.
- [ ] P9.5 Xác nhận person-first-seen → first recognition tăng không quá 200 ms so baseline.
- [ ] P9.6 Xác nhận idle `awnn_run` giảm ít nhất 70% mà không tăng missed-person.
- [ ] P9.7 Test IoT offline từ startup, stale state, reconnect và restart.
- [ ] P9.8 Test nhiều người: resolved resident không chặn unresolved/new person.
- [ ] P9.9 Test service modes bằng fake server/simulator.
- [ ] P9.10 Soak test tối thiểu 30 phút idle và nhiều cabin visit; theo dõi nhiệt độ/RAM/FPS.

## P10 — Rollout

- [ ] P10.1 Deploy shadow mode trên thang máy số 2.
- [ ] P10.2 Bật SCRFD idle gating, giữ YOLO baseline.
- [ ] P10.3 Bật adaptive YOLO cadence và degraded fallback.
- [ ] P10.4 Bật per-track face/recognition gating.
- [ ] P10.5 Bật service-mode inhibit cho floor command sau khi P0 hoàn tất.
- [ ] P10.6 Theo dõi metrics, false negative và rollback drill.

## Thứ tự triển khai đề xuất

1. Hoàn tất P0 và đo baseline P1.
2. Làm P2/P3/P4 với unit tests, chưa chạm pipeline thật.
3. Tích hợp state event P5 và chạy shadow mode.
4. Tích hợp inference P6 theo từng stage: SCRFD trước, YOLO sau.
5. Nối floor policy P7 sau khi state contract ổn định.
6. Hoàn thiện config/metrics P8, verification P9 và rollout P10.
