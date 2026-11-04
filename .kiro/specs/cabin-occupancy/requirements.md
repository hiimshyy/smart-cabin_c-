# Requirements Document

## Introduction

Spec này mô tả tính năng **Đếm người trong cabin bằng Counting Zone**.

Camera cabin có góc nhìn hạn chế và góc chết, không bao phủ toàn bộ cabin. Không thể dùng tổng số người detect trong một frame làm số người hiện tại. Thay vào đó, hệ thống đếm từng người khi họ đi qua vùng cửa thang máy.

## Glossary

- **Counting zone:** polygon trên ảnh camera, đặt tại vùng cửa thang máy, dùng để phát hiện người đi vào hoặc ra khỏi cabin.
- **Person track:** người được tracker gán một `track.id` và theo dõi qua nhiều frame.
- **Điểm tham chiếu (điểm chân):** điểm bottom-center của bbox person track.
- **Hướng vào cabin:** hướng từ phía ngoài vào phía trong cabin, cấu hình cùng polygon; hai thứ này chia không gian thành phía ngoài, trong zone và phía trong cabin.
- **Displacement:** quãng dịch chuyển của điểm chân theo hướng vào cabin, tính theo tọa độ chuẩn hóa. Crossing chỉ hợp lệ khi đạt ngưỡng cấu hình.
- **Entry / Exit:** crossing hợp lệ ngoài → zone → trong cabin (tăng count) / trong cabin → zone → ngoài (giảm count).
- **Aborted crossing:** track vào zone rồi quay lại cùng phía ban đầu.
- **Occupancy:** số người hiện tại trong cabin (`count`) cùng mức tin cậy (`confidence`).
- **`Certain` / `Uncertain`:** giá trị của `confidence`. `Uncertain` nghĩa là `count` có thể đã lệch so với thực tế.
- **Prediction-only:** track trong frame hiện tại chỉ có vị trí dự đoán của tracker, không có detection quan sát được.
- **Track-ID churn:** tracker đổi ID của cùng một người (ID cũ biến mất, ID mới xuất hiện ở gần).
- **Ghost state:** state giữ lại trong thời gian ngắn cho track vừa mất, để một ID mới ở gần có thể kế thừa thay vì bị đếm lại.
- **Door degraded:** door scheduler báo trạng thái `Degraded`; khi đó tracking được coi là không tin cậy.
- **Reconcile:** đặt lại `count` từ một nguồn bên ngoài và đưa `confidence` về `Certain`.
- **Generation:** số tăng dần mỗi lần state occupancy được cập nhật.

## Requirements

### Requirement 1: Counting zone

**User Story:** Là kỹ sư lắp đặt, tôi muốn cấu hình counting zone cho từng camera trong một file JSON riêng, để đặt zone đúng vùng cửa thang máy mà không cần build lại app.

#### Acceptance Criteria

1. Cấu hình vùng đếm là polygon trên một file JSON riêng cho từng camera.
2. File JSON có version, lưu tọa độ chuẩn hóa `[0,1]` cùng kích thước frame gốc (`reference_width/height`). Runtime chuyển sang pixel.
3. Validate khi load: version hợp lệ, polygon ≥ 3 điểm, tọa độ trong `[0,1]`, hướng vào cabin hợp lệ.
4. Ghi file config an toàn: không ghi đè file hợp lệ nếu lưu thất bại.

### Requirement 2: Đếm event-based

**User Story:** Là người vận hành, tôi muốn hệ thống đếm từng người khi họ đi qua vùng cửa thang máy, để count không phụ thuộc vào số người camera nhìn thấy trong một frame.

#### Acceptance Criteria

1. Dùng điểm dưới chân (bottom-center bbox) của mỗi person track làm điểm tham chiếu.
2. Phát hiện entry khi track đi từ ngoài → trong zone → vào trong cabin đủ displacement.
3. Phát hiện exit khi track đi từ trong cabin → trong zone → ra ngoài.
4. Không đếm khi track vào zone rồi lùi lại cùng phía (aborted crossing).
5. Không dùng door register `8472-2` để chứng minh cabin trống.

### Requirement 3: Chống drift và xử lý bất định

**User Story:** Là người vận hành, tôi muốn count không tự reset và được đánh dấu `Uncertain` khi tracking không tin cậy, để biết khi nào count có thể đã lệch so với thực tế.

#### Acceptance Criteria

1. Không tự reset occupancy khi mất track, timeout, hay cửa đóng.
2. Giữ occupancy cuối cùng; đánh dấu `Uncertain` khi RTSP outage, tracking chỉ còn prediction, track lost trong zone, track ID churn không rõ, hoặc crossing bị abort.
3. `Certain` trở lại khi có entry/exit validated hoặc reconcile.
4. Xử lý track-ID churn bằng ghost state ngắn hạn để tránh double count.

### Requirement 4: Persistence

**User Story:** Là người vận hành, tôi muốn occupancy được lưu xuống đĩa, để khởi động lại app không làm mất count.

#### Acceptance Criteria

1. Lưu occupancy (`count`, `confidence`, `generation`, `saved_at_ms`) ra file JSON nhỏ.
2. Ghi atomic (temp → fsync → rename).
3. Khởi động lại app phải phục hồi occupancy; nếu không có file hoặc file corrupt thì `count=0, confidence=Uncertain` và log warning.

### Requirement 5: Tích hợp

**User Story:** Là người vận hành, tôi muốn cấu hình đường dẫn occupancy qua CLI và `env.sh`, xem occupancy trên HUD và log, và có API reconcile cho nguồn bên ngoài, để triển khai, theo dõi và hiệu chỉnh count giống các tính năng hiện có.

#### Acceptance Criteria

1. CLI `--occupancy-config PATH` và `--occupancy-state PATH`.
2. `env.sh` tạo đường dẫn mặc định theo `FACE_CABIN_ID`.
3. Hiển thị occupancy trên preview HUD và log `sys` metrics.
4. Cung cấp API `reconcile(count)` / `reconcile_empty()` cho nguồn bên ngoài trong tương lai.

### Requirement 6: Tests

**User Story:** Là developer, tôi muốn có test cho config, persistence và các tình huống đếm, để phát hiện regression trước khi deploy.

#### Acceptance Criteria

1. Load/save/validate config.
2. Persistence corruption và failed-save không clobber.
3. Entry/exit, duplicate, aborted, track loss, RTSP outage, ID churn, door degraded, reconcile empty.

## Known limitations

- Zone phải phủ hết cửa camera nhìn thấy; nếu camera không thấy một phần cửa thì count vẫn có thể drift và chuyển Uncertain.
- Nhiều người chồng lấn thành một track sẽ bị đếm thiếu — cần reconcile ngoài.
