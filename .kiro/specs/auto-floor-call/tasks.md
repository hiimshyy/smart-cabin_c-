# Implementation Plan: Xây lại logic gọi tầng tự động (auto-floor-call)

## Overview

Trạng thái: **Chưa triển khai**
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

Kế hoạch thay việc gọi trực tiếp `send_floor(hf)` bằng `FloorCallCoordinator`: coordinator giữ ngữ cảnh phiên cabin dựa trên presence từ person tracker, chống gọi trùng theo `resident_id` và theo tầng trong toàn bộ phiên, còn `SocketIoClient` chỉ truyền từng command `2^floor` có `requestId` và trả kết quả ACK nghiệp vụ.

Thứ tự đi từ hợp đồng IoT Core (task 0), logic thuần có unit test (task 1–2), tích hợp recognition (task 3), transport (task 4), audit và cấu hình (task 5–6), unit tests bắt buộc (task 7), rồi mới tới integration/live test (task 8) và rollout từng bước (task 9).

## Tasks

- [ ] 0. Chốt hợp đồng IoT Core
  - [x] 0.1 Xác nhận write `8193-2=2^floor` set thêm một tầng; write sau không xóa tầng đã chọn.
  - [ ] 0.2 Lấy các ACK mẫu: success, already-selected, invalid value, device offline, forbidden mode.
  - [ ] 0.3 Chốt schema ACK có `requestId`, `success`, `code`, `message`.
  - [ ] 0.4 Xác nhận IoT Core có idempotency theo `requestId` và retention bao lâu.
  - [x] 0.5 Không cần read-modify-write hoặc merge bit trong AI Core; board giữ các tầng qua từng write.

- [x] 1. Coordinator thuần logic
  - [x] 1.1 Tạo `src/floor_call_coordinator.h/.cpp`, không phụ thuộc socket hay DB.
  - [x] 1.2 Thêm `CabinVisit` với state `EMPTY`, `ACTIVE`, `EMPTY_GRACE`.
  - [x] 1.3 Sinh `visit_id` monotonic và `request_id` duy nhất theo cabin/visit/sequence.
  - [x] 1.4 Thêm dedupe theo `resident_id` trong toàn bộ phiên.
  - [x] 1.5 Thêm dedupe theo `floor` trong toàn bộ phiên.
  - [x] 1.6 Validate active resident, no-floor, cabin range và encoder range.
  - [x] 1.7 Giữ snapshot resident/floor tại thời điểm quyết định.
  - [x] 1.8 Giới hạn số command trên mỗi phiên.
  - [x] 1.9 Phát decision event cho mọi accepted/suppressed/rejected outcome.

- [ ] 2. Presence và biên phiên
  - [x] 2.1 Tạo `PresenceSnapshot` từ person tracker, gồm track `Tracked` và `Lost` còn trong grace.
  - [x] 2.2 Gọi `on_presence()` mỗi frame kể cả khi không thấy khuôn mặt.
  - [x] 2.3 Bắt đầu phiên khi người đầu tiên xuất hiện.
  - [x] 2.4 Kết thúc phiên sau `floor_session_empty_ms` liên tục không có người.
  - [x] 2.5 Validate `floor_session_empty_ms >= track_lost_ms`.
  - [ ] 2.6 Log start/end của phiên và duration, không log PII.

- [ ] 3. Tích hợp recognition
  - [ ] 3.1 Tạo `ConfirmedIdentity` từ outcome và resident catalog snapshot.
  - [ ] 3.2 Sort outcomes cùng frame theo timestamp, resident ID, subject key.
  - [x] 3.3 Thay gọi trực tiếp `send_floor(hf)` bằng `coordinator.on_confirmed()`.
  - [ ] 3.4 Đảm bảo identity latch không bị xóa do một frame không match hoặc người quay mặt đi.
  - [x] 3.5 Giữ unknown/no-floor chỉ là decision, không gửi command.
  - [x] 3.6 Tách recognition cooldown khỏi floor-call dedupe.
  - [ ] 3.7 Xử lý catalog invalidation cho command chưa gửi theo R7.

- [ ] 4. Nâng cấp SocketIoClient
  - [x] 4.1 Queue một command có `request_id`, floor, value, timestamps thay vì chỉ floor/value.
  - [x] 4.2 Thêm callback trạng thái `SENT`, `SUCCESS`, `DUPLICATE`, `ERROR`, `EXPIRED`, `AMBIGUOUS`.
  - [x] 4.3 Nhả mutex trước khi gọi callback.
  - [x] 4.4 Parse payload ACK và kiểm tra cả Socket.IO `ack_id` lẫn nghiệp vụ `requestId`.
  - [x] 4.5 Không tăng `acked_success` cho ACK malformed hoặc `success=false`.
  - [ ] 4.6 Giữ một command in-flight; command khác FIFO chờ kết quả.
  - [x] 4.7 Command hết TTL trước send thành `EXPIRED`.
  - [x] 4.8 Disconnect/timeout sau send thành `AMBIGUOUS`; mặc định không retry.
  - [ ] 4.9 Chỉ thêm retry sau khi task 0.4 xác nhận idempotency; retry dùng cùng request ID.
  - [ ] 4.10 Giữ terminal log packet TX phục vụ live test, không chứa tên.

- [ ] 5. Audit và quan sát
  - [x] 5.1 Không gán `match_events.floor_selected` tại thời điểm enqueue.
  - [x] 5.2 Chọn migration `floor_call_events` hoặc structured log cho phase đầu.
  - [x] 5.3 Ghi lifecycle theo `visit_id` và `request_id`.
  - [ ] 5.4 Thêm metrics queued, sent, ack_success, ack_error, expired, ambiguous và suppressed.
  - [ ] 5.5 Log reason cho queue full, invalid floor, same resident, same floor và policy inhibit.

- [ ] 6. Cấu hình
  - [ ] 6.1 Thêm `floor_call_enabled` và `floor_call_shadow` từ DB/env/CLI theo precedence hiện tại.
  - [x] 6.2 Thêm `floor_session_empty_ms`, mặc định 5000.
  - [x] 6.3 Dùng `floor_command_ttl_ms` và `floor_ack_timeout_ms` thay magic values.
  - [ ] 6.4 Thêm `floor_dedupe_by_floor`, mặc định true.
  - [x] 6.5 Thêm `floor_max_commands_per_visit`, mặc định 16.
  - [ ] 6.6 In effective config khi startup, không in secret/RTSP credentials.

- [ ] 7. Unit tests bắt buộc
  - [x] 7.1 Một resident tầng 5 sinh đúng một command giá trị 32.
  - [x] 7.2 Recognition lặp và track ID thay đổi không sinh command thứ hai trong phiên.
  - [x] 7.3 Hai resident cùng tầng chỉ sinh một command.
  - [x] 7.4 Hai resident khác tầng sinh hai command theo thứ tự ổn định.
  - [x] 7.5 Unknown, no-floor và invalid-floor không gửi.
  - [x] 7.6 Empty ngắn hơn timeout giữ phiên; empty đủ timeout mở quyền cho phiên mới.
  - [ ] 7.7 Queue full, expiry và command limit có reason đúng.
  - [x] 7.8 ACK success/duplicate/error/malformed được phân loại đúng.
  - [ ] 7.9 Disconnect trước send giữ command còn TTL; sau send tạo ambiguous.
  - [ ] 7.10 Callback reentrant không deadlock.
  - [ ] 7.11 Catalog change không âm thầm sửa command snapshot.
  - [ ] 7.12 Floor call không phụ thuộc cooldown 3 giây của InteractionManager.

- [ ] 8. Integration và live tests
  - [ ] 8.1 Shadow test bằng video: một người quay đầu, che mặt và bị đổi track.
  - [ ] 8.2 Shadow test nhiều người cùng tầng và khác tầng.
  - [ ] 8.3 Test một resident trên thang máy số 2: `8193-2`, floor 5, value 32.
  - [ ] 8.4 Đối chiếu TX, ACK payload, audit state và nút tầng thực tế.
  - [ ] 8.5 Tắt mạng trước send, sau send và khi chờ ACK.
  - [ ] 8.6 Test IoT Core restart trong phiên.
  - [ ] 8.7 Test camera dropout ngắn không đóng cabin visit.
  - [ ] 8.8 Xác nhận FPS không giảm đáng kể khi command/ACK hoạt động.
  - [ ] 8.9 Test nhiều tầng thật: gửi `32`, sau đó `64`, xác nhận board giữ cả tầng 5 và tầng 6.

- [ ] 9. Rollout
  - [ ] 9.1 Deploy shadow mode và thu metrics/log theo phiên.
  - [ ] 9.2 Review false duplicate, missed call và session boundary từ dữ liệu test.
  - [ ] 9.3 Bật command thật cho một cabin/cư dân kiểm soát.
  - [ ] 9.4 Mở rộng same-floor, sau đó multi-floor.
  - [ ] 9.5 Đặt rollback bằng config: tắt `floor_call_enabled`, không cần build lại.

## Notes

### Thứ tự triển khai đề xuất

1. Hoàn tất task 0.2–0.3 song song với task 1, task 2 và unit tests thuần logic; task 0.1 đã được xác nhận.
2. Tích hợp task 3 ở shadow mode.
3. Nâng transport task 4 và audit task 5.
4. Chạy toàn bộ task 7, rồi task 8 theo từng cấp rủi ro.
5. Chỉ bật mặc định sau khi dữ liệu task 9.2 đạt tiêu chí vận hành.

### Về Task Dependency Graph

- Graph dựa trên luồng dữ liệu trong `design.md`: `CabinVisit` (1.2) là nền của sinh ID, dedupe và giới hạn command (1.3–1.5, 1.8); decision event (1.9) cần mọi nhánh quyết định; `main.cpp` gọi `on_confirmed()` (3.3) cần coordinator (1.9) và transport queue mới (4.1); parse ACK (4.4) cần schema ACK đã chốt (0.3).
- Task 2.5 cần cấu hình `floor_session_empty_ms` (6.2). Task 6.3 thay magic value của TTL/ACK timeout đã dùng ở 4.7 và 4.8. Task 6.6 đứng sau mọi task cấu hình khác.
- Task 4.9 chỉ phụ thuộc 0.4 và 4.8. Retry là tùy chọn, nên task 0.4 và 4.9 không chặn task 8.x.
- Mỗi unit test 7.x đứng sau phần code nó kiểm. Task 7.4 kiểm thứ tự ổn định (Property 6), nên cần sort outcome (3.2).
- Task 8.x chạy sau toàn bộ task 1–7 (trừ 4.9), theo cấp rủi ro: shadow (8.1, 8.2, 8.7), một resident thật (8.3, rồi 8.4, 8.8), nhiều tầng thật (8.9), cuối cùng fault injection (8.5, 8.6).
- Task 9.x tuần tự. Rollback bằng config (9.5) phải sẵn sàng trước khi bật command thật (9.3).

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["0.1", "0.2", "0.4", "1.1", "3.4", "4.1", "5.2", "6.1", "6.2"] },
    { "id": 1, "tasks": ["0.3", "0.5", "1.2", "1.6", "1.7", "2.1", "4.2", "4.10"] },
    { "id": 2, "tasks": ["1.3", "1.4", "1.5", "1.8", "2.2", "2.3", "3.1", "4.3", "4.4", "4.6", "4.7", "4.8", "7.5"] },
    { "id": 3, "tasks": ["1.9", "2.4", "3.2", "3.7", "4.5", "4.9", "5.3", "6.3", "6.4", "6.5", "7.1", "7.2", "7.3", "7.9", "7.10"] },
    { "id": 4, "tasks": ["2.5", "2.6", "3.3", "5.4", "5.5", "6.6", "7.4", "7.6", "7.7", "7.8", "7.11"] },
    { "id": 5, "tasks": ["3.5", "3.6", "5.1"] },
    { "id": 6, "tasks": ["7.12"] },
    { "id": 7, "tasks": ["8.1", "8.2", "8.7"] },
    { "id": 8, "tasks": ["8.3"] },
    { "id": 9, "tasks": ["8.4", "8.8"] },
    { "id": 10, "tasks": ["8.9"] },
    { "id": 11, "tasks": ["8.5", "8.6"] },
    { "id": 12, "tasks": ["9.1"] },
    { "id": 13, "tasks": ["9.2", "9.5"] },
    { "id": 14, "tasks": ["9.3"] },
    { "id": 15, "tasks": ["9.4"] }
  ]
}
```
