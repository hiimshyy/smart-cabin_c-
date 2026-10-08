# Tasks: Xây lại logic gọi tầng tự động

Trạng thái: **Chưa triển khai**
Yêu cầu: [requirements.md](requirements.md)
Thiết kế: [design.md](design.md)

## P0 — Chốt hợp đồng IoT Core

- [x] P0.1 Xác nhận write `8193-2=2^floor` set thêm một tầng; write sau không xóa tầng đã chọn.
- [ ] P0.2 Lấy các ACK mẫu: success, already-selected, invalid value, device offline, forbidden mode.
- [ ] P0.3 Chốt schema ACK có `requestId`, `success`, `code`, `message`.
- [ ] P0.4 Xác nhận IoT Core có idempotency theo `requestId` và retention bao lâu.
- [x] P0.5 Không cần read-modify-write hoặc merge bit trong AI Core; board giữ các tầng qua từng write.

## P1 — Coordinator thuần logic

- [x] P1.1 Tạo `src/floor_call_coordinator.h/.cpp`, không phụ thuộc socket hay DB.
- [x] P1.2 Thêm `CabinVisit` với state `EMPTY`, `ACTIVE`, `EMPTY_GRACE`.
- [x] P1.3 Sinh `visit_id` monotonic và `request_id` duy nhất theo cabin/visit/sequence.
- [x] P1.4 Thêm dedupe theo `resident_id` trong toàn bộ phiên.
- [x] P1.5 Thêm dedupe theo `floor` trong toàn bộ phiên.
- [x] P1.6 Validate active resident, no-floor, cabin range và encoder range.
- [x] P1.7 Giữ snapshot resident/floor tại thời điểm quyết định.
- [x] P1.8 Giới hạn số command trên mỗi phiên.
- [x] P1.9 Phát decision event cho mọi accepted/suppressed/rejected outcome.

## P2 — Presence và biên phiên

- [x] P2.1 Tạo `PresenceSnapshot` từ person tracker, gồm track `Tracked` và `Lost` còn trong grace.
- [x] P2.2 Gọi `on_presence()` mỗi frame kể cả khi không thấy khuôn mặt.
- [x] P2.3 Bắt đầu phiên khi người đầu tiên xuất hiện.
- [x] P2.4 Kết thúc phiên sau `floor_session_empty_ms` liên tục không có người.
- [x] P2.5 Validate `floor_session_empty_ms >= track_lost_ms`.
- [ ] P2.6 Log start/end của phiên và duration, không log PII.

## P3 — Tích hợp recognition

- [ ] P3.1 Tạo `ConfirmedIdentity` từ outcome và resident catalog snapshot.
- [ ] P3.2 Sort outcomes cùng frame theo timestamp, resident ID, subject key.
- [x] P3.3 Thay gọi trực tiếp `send_floor(hf)` bằng `coordinator.on_confirmed()`.
- [ ] P3.4 Đảm bảo identity latch không bị xóa do một frame không match hoặc người quay mặt đi.
- [x] P3.5 Giữ unknown/no-floor chỉ là decision, không gửi command.
- [x] P3.6 Tách recognition cooldown khỏi floor-call dedupe.
- [ ] P3.7 Xử lý catalog invalidation cho command chưa gửi theo R7.

## P4 — Nâng cấp SocketIoClient

- [x] P4.1 Queue một command có `request_id`, floor, value, timestamps thay vì chỉ floor/value.
- [x] P4.2 Thêm callback trạng thái `SENT`, `SUCCESS`, `DUPLICATE`, `ERROR`, `EXPIRED`, `AMBIGUOUS`.
- [x] P4.3 Nhả mutex trước khi gọi callback.
- [x] P4.4 Parse payload ACK và kiểm tra cả Socket.IO `ack_id` lẫn nghiệp vụ `requestId`.
- [x] P4.5 Không tăng `acked_success` cho ACK malformed hoặc `success=false`.
- [ ] P4.6 Giữ một command in-flight; command khác FIFO chờ kết quả.
- [x] P4.7 Command hết TTL trước send thành `EXPIRED`.
- [x] P4.8 Disconnect/timeout sau send thành `AMBIGUOUS`; mặc định không retry.
- [ ] P4.9 Chỉ thêm retry sau khi P0.4 xác nhận idempotency; retry dùng cùng request ID.
- [ ] P4.10 Giữ terminal log packet TX phục vụ live test, không chứa tên.

## P5 — Audit và quan sát

- [x] P5.1 Không gán `match_events.floor_selected` tại thời điểm enqueue.
- [x] P5.2 Chọn migration `floor_call_events` hoặc structured log cho phase đầu.
- [x] P5.3 Ghi lifecycle theo `visit_id` và `request_id`.
- [ ] P5.4 Thêm metrics queued, sent, ack_success, ack_error, expired, ambiguous và suppressed.
- [ ] P5.5 Log reason cho queue full, invalid floor, same resident, same floor và policy inhibit.

## P6 — Cấu hình

- [ ] P6.1 Thêm `floor_call_enabled` và `floor_call_shadow` từ DB/env/CLI theo precedence hiện tại.
- [x] P6.2 Thêm `floor_session_empty_ms`, mặc định 5000.
- [x] P6.3 Dùng `floor_command_ttl_ms` và `floor_ack_timeout_ms` thay magic values.
- [ ] P6.4 Thêm `floor_dedupe_by_floor`, mặc định true.
- [x] P6.5 Thêm `floor_max_commands_per_visit`, mặc định 16.
- [ ] P6.6 In effective config khi startup, không in secret/RTSP credentials.

## P7 — Unit tests bắt buộc

- [x] P7.1 Một resident tầng 5 sinh đúng một command giá trị 32.
- [x] P7.2 Recognition lặp và track ID thay đổi không sinh command thứ hai trong phiên.
- [x] P7.3 Hai resident cùng tầng chỉ sinh một command.
- [x] P7.4 Hai resident khác tầng sinh hai command theo thứ tự ổn định.
- [x] P7.5 Unknown, no-floor và invalid-floor không gửi.
- [x] P7.6 Empty ngắn hơn timeout giữ phiên; empty đủ timeout mở quyền cho phiên mới.
- [ ] P7.7 Queue full, expiry và command limit có reason đúng.
- [x] P7.8 ACK success/duplicate/error/malformed được phân loại đúng.
- [ ] P7.9 Disconnect trước send giữ command còn TTL; sau send tạo ambiguous.
- [ ] P7.10 Callback reentrant không deadlock.
- [ ] P7.11 Catalog change không âm thầm sửa command snapshot.
- [ ] P7.12 Floor call không phụ thuộc cooldown 3 giây của InteractionManager.

## P8 — Integration và live tests

- [ ] P8.1 Shadow test bằng video: một người quay đầu, che mặt và bị đổi track.
- [ ] P8.2 Shadow test nhiều người cùng tầng và khác tầng.
- [ ] P8.3 Test một resident trên thang máy số 2: `8193-2`, floor 5, value 32.
- [ ] P8.4 Đối chiếu TX, ACK payload, audit state và nút tầng thực tế.
- [ ] P8.5 Tắt mạng trước send, sau send và khi chờ ACK.
- [ ] P8.6 Test IoT Core restart trong phiên.
- [ ] P8.7 Test camera dropout ngắn không đóng cabin visit.
- [ ] P8.8 Xác nhận FPS không giảm đáng kể khi command/ACK hoạt động.
- [ ] P8.9 Test nhiều tầng thật: gửi `32`, sau đó `64`, xác nhận board giữ cả tầng 5 và tầng 6.

## P9 — Rollout

- [ ] P9.1 Deploy shadow mode và thu metrics/log theo phiên.
- [ ] P9.2 Review false duplicate, missed call và session boundary từ dữ liệu test.
- [ ] P9.3 Bật command thật cho một cabin/cư dân kiểm soát.
- [ ] P9.4 Mở rộng same-floor, sau đó multi-floor.
- [ ] P9.5 Đặt rollback bằng config: tắt `floor_call_enabled`, không cần build lại.

## Thứ tự triển khai đề xuất

1. Hoàn tất P0.2–P0.3 song song với P1, P2 và unit tests thuần logic; P0.1 đã được xác nhận.
2. Tích hợp P3 ở shadow mode.
3. Nâng transport P4 và audit P5.
4. Chạy toàn bộ P7, rồi P8 theo từng cấp rủi ro.
5. Chỉ bật mặc định sau khi dữ liệu P9.2 đạt tiêu chí vận hành.
