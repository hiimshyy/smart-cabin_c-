# Thiết kế

## Phạm vi v1

V1 chỉ hỗ trợ **đúng một counting zone cho mỗi camera**. File JSON vẫn dùng mảng `zones` để giữ khả năng mở rộng định dạng, nhưng validator từ chối rõ ràng khi mảng có 0 hoặc nhiều hơn 1 phần tử. Runtime không bao giờ âm thầm dùng `zones.front()` khi cấu hình có nhiều zone.

## Module

- `occupancy_config`: định nghĩa `OccupancyConfig` và `CountingZone`, parse/serialize JSON tối thiểu, validate và đổi tọa độ chuẩn hóa sang pixel.
- `occupancy_state`: giữ `OccupancyState` (`count`, `confidence`, `generation`, `saved_at_ms`) và worker persistence bất đồng bộ.
- `occupancy_counter`: state machine theo `track.id`, phát `OccupancyEvent`, quản lý ghost ngắn hạn chống ID churn và callback mỗi khi trạng thái cần persistence.
- `app_config`, `env.sh`: bật tính năng bằng `--occupancy-config`; `--occupancy-state` là tùy chọn và mặc định thành `<config>.state`.
- `overlay`, `system_metrics`: chỉ hiển thị/phát occupancy khi tính năng đã được khởi tạo thành công.

## Cấu hình counting zone

JSON v1 có `version`, `reference_width`, `reference_height` và đúng một phần tử trong `zones`. Mỗi zone có tên, polygon ít nhất 3 điểm với diện tích khác 0, `inside_direction_degrees` hữu hạn trong `[0, 360)` và `min_crossing_displacement` hữu hạn trong `[0, 1]`. Mọi tọa độ phải hữu hạn và nằm trong `[0, 1]`; các trường nguyên không chấp nhận số thập phân hoặc số vượt miền.

Parser số JSON không ném exception khi gặp exponent malformed hoặc overflow. Save config dùng file tạm duy nhất, ghi đầy đủ, `fsync`, đóng, `rename` và `fsync` thư mục; target hợp lệ không bị thay thế nếu lỗi xảy ra trước `rename`.

## State machine mỗi track

Điểm tham chiếu là bottom-center của bbox. Polygon và vector hướng vào cabin chia không gian thành:

- `-1`: phía ngoài cabin;
- `0`: trong counting zone hoặc dải sticky cạnh zone;
- `+1`: phía trong cabin.

Các transition chính:

- outside → zone: bắt đầu entry candidate;
- entry candidate → inside đủ displacement: phát Entry, tăng count và chuyển `Certain`;
- inside → zone: bắt đầu exit candidate;
- exit candidate → outside đủ displacement: chỉ phát Exit và giảm count khi track đang giữ một occupancy slot đã được gán; track xuất hiện phía trong mà không có Entry/slot tương ứng chỉ giữ count, chuyển `Uncertain` và không phát event;
- candidate quay lại cùng phía: không đếm và chuyển `Uncertain`; candidate đi ra ngoài polygon qua dải sticky theo phương ngang vẫn được giữ cho đến khi projection xác nhận phía cuối, tránh hủy crossing hợp lệ ở polygon phối cảnh;
- mất track trong zone, mất track phía trong, observation prediction-only, door scheduler `Degraded`, frame invalid/RTSP outage hoặc ID churn mơ hồ: giữ nguyên count và chuyển `Uncertain`.

Do capture thread dùng latest-frame slot, live RTSP có thể bỏ frame trung gian khi inference chậm hơn camera. Nếu hai observation liên tiếp nhảy trực tiếp `outside ↔ inside`, counter kiểm tra đoạn nối hai điểm chân có thật sự cắt polygon và displacement có đạt ngưỡng hay không. Đoạn cắt hợp lệ được xử lý như crossing bình thường; đổi phía nhưng đi vòng ngoài polygon chỉ chuyển `Uncertain`, không phát event. Vì vậy replay đủ frame và live stream drop frame giữ cùng semantics.

ByteTrack vẫn giữ `confirm_hits=3` để chống detection nhiễu, nhưng `TrackView` của track đã xác nhận mang thêm bbox/timestamp của observation tentative đầu tiên. Trong tối đa 1 giây sau observation đầu, khi counter lần đầu nhận track đã xác nhận, nó khởi tạo state từ điểm chân đầu tiên rồi xử lý vị trí hiện tại như một transition bình thường. Nhờ vậy quỹ đạo `tentative outside → tentative zone → confirmed inside` hoặc `tentative outside → confirmed inside` vẫn tạo Entry; lịch sử cũ hơn 1 giây bị bỏ qua để không replay crossing sau reactivation/ghost expiry.

Ghost sống 500 ms kể từ lúc runtime phát hiện track biến mất. Nếu ID cũ và đúng một ID mới ở gần xuất hiện trong cùng frame boundary, counter chuyển state trực tiếp trước khi xử lý loss nên giữ nguyên count, confidence và không phát callback thừa. Sau khi đã thành ghost, một ID mới chỉ kế thừa khi có đúng một ghost phù hợp trong khoảng cách cho phép; nhiều predecessor/successor hoặc nhiều ghost phù hợp được coi là mơ hồ. Trường hợp một predecessor tách thành nhiều successor được phát hiện trước khi tạo/tiêu thụ ghost: confidence chuyển `Uncertain`, các lineage successor bị chặn phát crossing event trong vòng đời hiện tại để không nhân đôi count, và callback persistence chỉ chạy khi trạng thái thực sự đổi. Track mới không kế thừa ghost mà lần đầu xuất hiện trong zone hoặc phía cabin cũng chuyển `Uncertain`, giữ nguyên count và không tự tạo entry. Crossing hợp lệ từ lineage tin cậy hoặc `reconcile` đưa confidence về `Certain`.

Mỗi track còn giữ cờ gán occupancy riêng. Entry hợp lệ tạo đúng một slot gắn với track; chỉ track/ghost kế thừa slot đó mới được decrement và phát Exit. Count phục hồi từ state bắt đầu dưới dạng các slot chưa gán, có thể gán cho track đầu tiên quan sát đáng tin cậy ở phía trong cabin; slot của ghost hết hạn được trả về nhóm chưa gán. Nhờ vậy track mới xuất hiện phía trong khi count bằng 0, hoặc track phụ xuất hiện cạnh một người đã được đếm, không thể phát nhiều Exit hay trừ occupancy của track khác.

## Persistence

`OccupancyStateHolder::load` validate đầy đủ version, count không âm, confidence, generation không âm và timestamp hữu hạn. File thiếu hoặc invalid đều reset về `count=0`, `Uncertain`, trả status quan sát được và log warning qua `src/log/logger.h` mà không ghi PII.

`request_save` không làm disk I/O trên frame thread. Nó cập nhật snapshot trong một pending slot dung lượng 1; request mới thay snapshot cũ (bounded/drop-oldest). Worker riêng ghi snapshot mới nhất tối đa một lần mỗi giây bằng chuỗi: mở và kiểm tra thư mục cha trước khi thay target → temp duy nhất → write đầy đủ → `fsync` file → close → `rename` → bắt buộc `fsync` directory đã mở. Lỗi trước rename giữ nguyên target và cleanup temp; lỗi mở/fsync directory được trả về thay vì báo thành công. `close()` flush pending snapshot theo throttle rồi join worker an toàn. Validated entry/exit, mọi thay đổi confidence và cả `reconcile(count)`/`reconcile_empty()` đều đi qua callback để schedule persistence.

`saved_at_ms` dùng wall clock để có ý nghĩa sau restart; timestamp dùng cho tracking vẫn là steady clock.

## Tích hợp main

Occupancy chỉ được khởi tạo sau khi person tracker đã hợp lệ và config load/validate thành công. `env.sh` tạo config/state path từ `FACE_CABIN_ID`, chỉ bật nếu config tồn tại và truyền nguyên mảng `occupancy_args` có quote vào app.

Main loop ghép deadline occupancy tối đa 1 giây vào `wait_for_frame_or_deadline`. Tại mọi nhánh không có frame hoặc frame rỗng, counter nhận `frame_valid=false` trước `continue`; outage kéo dài vì vậy chuyển sang `Uncertain` mà không reset count. Với frame hợp lệ, update chạy sau `active_tracks()` tại frame boundary và nhận cả `predicted_only` lẫn cờ door degraded. Adapter greedy đánh dấu prediction-only khi track không được quan sát trong frame hiện tại, gồm frame bỏ qua detector và detection miss khi track vẫn còn confirmed.

HUD, polygon/direction overlay, frame metric `occ` và system metric `occupancy` chỉ xuất hiện khi feature enable thành công. System metrics đóng gói enabled/count/confidence vào một atomic snapshot duy nhất để worker không đọc cặp count/confidence bị xé. Shutdown enqueue snapshot cuối rồi flush/join worker trước khi logger dừng. Log event occupancy chỉ chứa numeric track ID.

## Kiểm thử

- `test_app_config`: option explicit/default và các lỗi CLI.
- `test_env_face_cabin.sh`: path theo cabin ID, quote path có khoảng trắng, argv thực tế và không lộ RTSP credential.
- `test_occupancy_config`: round-trip, numeric parser/validation, single-zone contract và failure injection cùng target cho temp open/write/fsync/rename, cleanup và directory durability.
- `test_occupancy_state`: missing/corrupt warning/reset, round-trip, atomic failure cùng target, pending-slot coalescing, throttle và shutdown flush.
- `test_occupancy_counter`: entry/exit, bounce/abort, loss, outage, prediction-only, degraded, clear/ambiguous ID churn (gồm confidence/callback) và reconcile callback.
- `test_greedy_adapter`: observation hiện tại, detector-skipped và detector-missed prediction-only.
- `test_video_wait`: deadline không-frame được nối tới update outage ở boundary có thể test.
- `test_overlay`, `test_system_metrics`: zone drawing, disabled/enabled HUD/metrics và atomic snapshot concurrency.

Đã replay qua NPU bằng hai recording camera thật. Video một chu kỳ đạt `0 → 1 → 0`; video fast-entry/lưỡng lự từng tái hiện lỗi và sau sửa sticky-band đạt `0 → 1 → 0 → 1 → 0` trên đủ 1.347 frame. Live camera trước bản sửa cuối đã giúp tái hiện lỗi, nhưng chưa live-retest sau bản sửa sticky-band cuối; stress test nhiều người và soak test service vẫn hoãn. Xem trạng thái bàn giao chi tiết tại `handoff.md`.
