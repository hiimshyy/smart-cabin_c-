# Requirements: Điều phối gọi tầng tự động

Trạng thái: **Draft để chốt nghiệp vụ trước khi sửa runtime**
Phạm vi: AI Core trong cabin, nhận diện cư dân và gửi lệnh gọi tầng qua Socket.IO tới IoT Core.

## 1. Mục tiêu

Hệ thống phải gọi đúng tầng của cư dân đã được nhận diện, chỉ gọi số lần cần thiết trong một lần sử
dụng cabin, không chặn luồng camera/inference và có đủ trạng thái để phân biệt lệnh mới xếp hàng,
đã gửi, được IoT Core xác nhận hay thất bại.

Thiết kế này tách ba trách nhiệm:

1. `InteractionManager` xác nhận danh tính từ kết quả nhận diện theo frame.
2. `FloorCallCoordinator` quyết định có nên gọi tầng trong phiên cabin hiện tại hay không.
3. `SocketIoClient` truyền lệnh và trả kết quả truyền về coordinator.

## 2. Các quyết định đã thống nhất

- AI Core gửi trực tiếp event `Client/command` tới IoT Core.
- Thiết bị: `deviceCode=2026010900100098`.
- Điều khiển thang máy số 2 tại `address=8193-2`.
- Giá trị gọi tầng là `2^floor`; ví dụ tầng 5 là `32`.
- Mỗi write sẽ set thêm tầng tương ứng trên board. Ghi `32` rồi `64` giữ đồng thời tầng 5 và tầng 6;
  write sau không thay thế các bit tầng đã chọn trước đó.
- `home_floor <= 0`, tầng ngoài phạm vi cabin, hoặc người chưa nhận diện được không tạo lệnh gọi tầng.
- Gửi mạng chạy bất đồng bộ, không được làm giảm FPS của pipeline camera/inference.
- Danh tính đã xác nhận được latch theo track; quay mặt đi tạm thời không làm mất tên và không tạo
  lại quyền gọi tầng.

## 3. Vấn đề của logic hiện tại

- Quyền gọi tầng đang gắn với `confirmed outcome` và cooldown 3 giây. Một người bị đổi track hoặc
  xuất hiện đủ lâu có thể sinh thêm outcome và gọi lại trong cùng lần đi thang máy.
- Không có khái niệm phiên cabin, nên không biết khi nào lịch sử chống gọi trùng được phép xóa.
- Hai cư dân cùng tầng có thể tạo hai lệnh giống nhau.
- `match_events.floor_selected` được ghi ngay khi lệnh vừa vào queue, trước khi gửi và trước ACK.
- ACK hiện chỉ được nhận diện bằng `ack_id`; nội dung ACK chưa được phân loại thành thành công hoặc
  lỗi nghiệp vụ.
- `requestId=ai-core` là cố định, không đủ để tương quan từng lệnh hoặc retry an toàn.

## 4. Định nghĩa

- **Phiên cabin (cabin visit):** khoảng thời gian bắt đầu khi hệ thống nhìn thấy ít nhất một người và
  kết thúc khi không còn người liên tục đủ `session_empty_ms`.
- **Người hiện diện:** track người còn ở trạng thái tracked/lost trong grace của tracker; không phụ
  thuộc việc frame hiện tại có nhìn thấy khuôn mặt hay không.
- **Resident đã xử lý:** resident đã có quyết định cuối trong phiên: đã tạo lệnh, được gộp vì cùng
  tầng, không có tầng, tầng không hợp lệ hoặc bị policy từ chối.
- **Tầng đã yêu cầu:** tầng đã có command ở một trong các trạng thái `QUEUED`, `SENT`,
  `ACKED_SUCCESS`, `ACKED_DUPLICATE` hoặc `AMBIGUOUS` trong phiên hiện tại.
- **ACK transport:** Socket.IO trả đúng `ack_id`; chỉ chứng minh có phản hồi.
- **ACK nghiệp vụ:** payload ACK cho biết write được chấp nhận, trùng, hay bị từ chối.

## 5. Yêu cầu chức năng

### R1 — Biên phiên cabin

1. Phiên bắt đầu khi có người hiện diện đầu tiên.
2. Mất detection ngắn, mất mặt do quay đầu hoặc track chuyển sang `Lost` không kết thúc phiên.
3. Phiên chỉ kết thúc khi số người hiện diện bằng 0 liên tục ít nhất `session_empty_ms`.
4. Mặc định đề xuất `session_empty_ms=5000`.
5. Khi phiên kết thúc, danh sách resident đã xử lý và tầng đã yêu cầu được xóa. Người đi vào sau đó
   được phép gọi tầng trong phiên mới.
6. Nếu sau này có dữ liệu cửa cabin tin cậy, một chu kỳ cửa đóng/mở có thể được thêm làm tín hiệu
   kết thúc phiên; timeout không người vẫn là đường dự phòng.

### R2 — Điều kiện tạo yêu cầu gọi tầng

Một confirmed recognition chỉ đủ điều kiện khi đồng thời thỏa:

- `resident_id` hợp lệ và resident vẫn tồn tại/active trong catalog;
- `home_floor > 0`;
- `floors_min <= home_floor <= floors_max`;
- tầng mã hóa được bằng kiểu register;
- resident chưa được xử lý trong phiên hiện tại;
- coordinator đang bật và cabin không ở chế độ chặn gọi tự động.

### R3 — Chống gọi trùng theo resident

1. Mỗi `resident_id` chỉ được quyết định một lần trong một phiên cabin, không phụ thuộc `track_id`.
2. Đổi track, tạo ghost track, quay mặt đi rồi quay lại hoặc recognition phát lại không được gọi lại.
3. Cooldown của `InteractionManager` vẫn có thể dùng để giảm event nhận diện, nhưng không được dùng
   làm cơ chế chống gọi tầng chính.

### R4 — Chống gọi trùng theo tầng

1. Trong một phiên cabin, mỗi tầng chỉ tạo tối đa một command.
2. Nếu resident B có cùng tầng với resident A đã yêu cầu, B được đánh dấu `SUPPRESSED_SAME_FLOOR`;
   không tạo thêm write.
3. Một command đang `AMBIGUOUS` vẫn giữ tầng trong tập đã yêu cầu, vì write có thể đã đến IoT Core.
4. Lỗi xảy ra trước khi gửi có thể cho phép một resident khác cùng tầng dùng lại yêu cầu, theo R9.

### R5 — Nhiều người và nhiều tầng

1. Nhiều tầng khác nhau được xếp FIFO theo thời điểm confirmed.
2. Nếu timestamp bằng nhau, dùng `resident_id` tăng dần để kết quả luôn xác định.
3. Socket client chỉ có tối đa một command đang chờ ACK.
4. Ví dụ A tầng 5 và B tầng 5: chỉ gửi giá trị `32`.
5. Ví dụ A tầng 5 rồi B tầng 6: gửi `32`, chờ kết quả, sau đó gửi `64`.
6. AI Core gửi từng tầng thành một write riêng. Board giữ các tầng đã chọn nên không cần OR nhiều
   tầng thành một command.

### R6 — Unknown và resident không có tầng

1. Unknown không tạo lệnh gọi tầng.
2. Resident có `home_floor=0` được ghi nhận `SUPPRESSED_NO_FLOOR` trong phiên và không bị kiểm tra/gọi
   lặp lại liên tục.
3. Resident có tầng ngoài phạm vi được ghi `REJECTED_INVALID_FLOOR`; hệ thống tiếp tục xử lý người
   khác.

### R7 — Snapshot dữ liệu resident

1. Khi tạo quyết định, coordinator giữ snapshot `resident_id`, `home_floor`, similarity và thời gian.
2. Catalog đổi `home_floor` sau khi command đã xếp hàng không được âm thầm đổi command đó.
3. Catalog invalidation được phép hủy command chưa gửi nếu resident bị xóa/deactivate; command đã gửi
   chuyển theo ACK hoặc `AMBIGUOUS` và không được gửi lệnh bù tự động.
4. Phiên cabin mới luôn dùng catalog mới nhất.

### R8 — Vòng đời command

Mỗi yêu cầu phải có các trạng thái sau:

- `ELIGIBLE`: đủ điều kiện, chưa vào transport queue;
- `QUEUED`: đã được queue tiếp nhận;
- `SENT`: frame Socket.IO đã được gửi;
- `ACKED_SUCCESS`: IoT Core xác nhận write thành công;
- `ACKED_DUPLICATE`: IoT Core cho biết tầng đã được chọn; coi là hoàn tất thành công;
- `ACKED_ERROR`: IoT Core từ chối hoặc báo lỗi;
- `EXPIRED`: hết TTL trước khi gửi;
- `AMBIGUOUS`: đã gửi nhưng timeout/mất kết nối trước ACK;
- `SUPPRESSED`: không cần gửi do trùng resident/tầng hoặc policy;
- `REJECTED`: dữ liệu đầu vào không hợp lệ.

`floor_selected` chỉ được coi là thành công tại `ACKED_SUCCESS` hoặc `ACKED_DUPLICATE`, không phải
tại `QUEUED`.

### R9 — Retry và lỗi

1. Command hết TTL trước khi gửi chuyển `EXPIRED`; không gửi một yêu cầu đã cũ khi kết nối phục hồi.
2. Command đã gửi nhưng không có ACK chuyển `AMBIGUOUS`; không tự retry với protocol hiện tại.
3. Khi IoT Core hỗ trợ `requestId` duy nhất và idempotency, có thể retry `AMBIGUOUS` với cùng ID.
4. `ACKED_ERROR` không retry vô hạn. Mặc định thử tối đa một lần cho lỗi được IoT Core đánh dấu
   retryable; lỗi validation/permission không retry.
5. Lỗi một command không dừng nhận diện hoặc làm mất những command khác còn hợp lệ.

### R10 — Correlation và ACK

1. Mỗi command cần ID duy nhất, đề xuất:
   `ai-core:<cabin_id>:<visit_id>:<sequence>`.
2. Socket.IO `ack_id` dùng để ghép phản hồi của kết nối hiện tại; `requestId` dùng để ghép nghiệp vụ
   qua reconnect.
3. IoT Core cần trả tối thiểu `requestId`, `success`, `code`, `message` hoặc schema tương đương đã
   được hai bên chốt.
4. Client phải parse nội dung ACK, không được coi mọi packet `43<ack_id>[...]` là write thành công.

### R11 — Audit và log

1. Ghi một event nhận diện khi danh tính được xác nhận.
2. Ghi riêng vòng đời gọi tầng, tối thiểu gồm `visit_id`, `request_id`, `resident_id`, `floor`,
   `value`, `state`, `reason`, timestamp và ACK code.
3. Log terminal có thể in payload gửi để test, nhưng không ghi tên hoặc dữ liệu nhạy cảm.
4. Metrics tối thiểu: queued, sent, ack_success, ack_error, expired, ambiguous,
   suppressed_same_resident, suppressed_same_floor.

### R12 — Không ảnh hưởng realtime

1. `FloorCallCoordinator::on_*` không thực hiện I/O mạng hoặc DB đồng bộ trong inference thread.
2. Queue có giới hạn; khi đầy phải trả kết quả rõ ràng để audit.
3. Callback trạng thái từ transport phải thread-safe và không giữ lock trong khi gọi ra component
   khác.

## 6. Kịch bản chấp nhận

| ID | Kịch bản | Kết quả mong đợi |
|---|---|---|
| S01 | A được confirm, tầng 5 | Một lệnh `8193-2=32` |
| S02 | A quay mặt đi rồi quay lại trong cùng phiên | Không gọi lại |
| S03 | A bị đổi từ track 10 sang 21 | Không gọi lại vì dedupe theo resident |
| S04 | A tầng 5 và B tầng 5 | Chỉ một lệnh `32`; B suppressed cùng tầng |
| S05 | A tầng 5 và B tầng 6 | Lần lượt `32`, rồi `64` |
| S06 | A unknown | Không có command |
| S07 | A có `home_floor=0` | Không command; ghi no-floor một lần trong phiên |
| S08 | A có tầng ngoài phạm vi cabin | Không command; ghi invalid-floor |
| S09 | Cabin trống 2 giây rồi A hiện lại | Vẫn cùng phiên; không gọi lại |
| S10 | Cabin trống quá 5 giây rồi A vào lại | Phiên mới; được gọi lại |
| S11 | Socket offline quá TTL | Command chưa gửi thành `EXPIRED` |
| S12 | Mất kết nối sau khi send, trước ACK | `AMBIGUOUS`; không tự gửi lại |
| S13 | ACK đúng id nhưng payload báo lỗi | `ACKED_ERROR`, không ghi floor thành công |
| S14 | ACK báo tầng đã được chọn | `ACKED_DUPLICATE`, coi hoàn tất |
| S15 | Queue đầy | Từ chối enqueue có reason; pipeline vẫn chạy |
| S16 | Catalog đổi tầng khi command đang queue | Command giữ snapshot cũ hoặc bị hủy rõ ràng theo R7 |
| S17 | Recognition lặp sau cooldown 3 giây | Coordinator vẫn không gọi lại trong cùng phiên |
| S18 | Hai confirmed cùng timestamp | Thứ tự ổn định theo resident_id |

## 7. Thông tin phải chốt với IoT Core

Semantics register đã được xác nhận: write là thao tác set thêm một tầng; ghi `32` rồi `64` giữ cả
tầng 5 và tầng 6. Các điểm còn lại cần chốt trước khi coi bản triển khai là production-ready:

1. Payload ACK thành công, trùng tầng và lỗi có schema chính xác như thế nào?
2. IoT Core có lưu và dedupe `requestId` không, thời gian lưu bao lâu?
3. Có trạng thái chuẩn hóa nào để chặn gọi trong maintenance, emergency, fire mode hoặc khi cửa/cabin
   ở trạng thái không phù hợp không?

## 8. Ngoài phạm vi phiên bản đầu

- Chọn tầng theo lịch, vai trò hoặc quyền truy cập ngoài `home_floor`.
- Hủy tầng đã bấm trên board.
- Tối ưu thứ tự tầng theo hướng chạy của thang máy.
- Điều khiển nhiều cabin từ một tiến trình AI Core.
- Dùng current floor/door/direction để dự đoán hành trình; có thể bổ sung sau qua event chuẩn hóa từ
  IoT Core.
