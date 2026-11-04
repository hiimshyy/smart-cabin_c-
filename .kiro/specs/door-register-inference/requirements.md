# Requirements Document

## Introduction

Spec này mô tả tính năng **Điều phối inference theo thanh ghi cửa 8472-2**.

Quyết định 2026-10-22: lấy dữ liệu từ `CONN_DATA` qua Socket.IO của iot_core theo
`/edge/native_core/socket_test.cpp`, slave id 2. Model vẫn load một lần tại startup.

Đợt này chưa thay đổi chính sách gửi lệnh gọi tầng. Cần smoke test board/camera thật trước deploy.

## Glossary

- **iot_core:** dịch vụ đọc board thang máy và gửi dữ liệu thanh ghi qua Socket.IO.
- **`CONN_DATA`:** event Socket.IO của iot_core chứa giá trị thanh ghi của board.
- **Slave id 2:** board thang máy mà spec này đọc dữ liệu cửa.
- **`8472-2`:** khóa thanh ghi cửa của slave 2 trong `CONN_DATA`; giá trị thô là số nguyên.
- **Bit 5 / bit 6:** bit trong giá trị thô `8472-2` báo mở cửa / đóng cửa.
- **Gói partial:** gói `CONN_DATA` chỉ chứa một phần thanh ghi, không có khóa `8472-2`.
- **YOLO:** model person detection.
- **SCRFD:** model face detection.
- **Recognition:** face recognition, chạy theo từng person track.
- **Person track:** track người do tracker duy trì từ detection của YOLO.
- **Empty grace:** khoảng chờ sau khi cửa đóng và cabin hết person track, trước khi các model ngừng
  inference.
- **Ngừng inference:** bỏ qua lần chạy model; model vẫn được load, không unload NPU context.
- **Quét YOLO dự phòng:** chạy YOLO thưa khi không tin được dữ liệu cửa, để vẫn phát hiện người.
- **Pipeline baseline:** lịch inference trước tính năng này, không phụ thuộc dữ liệu cửa.

## Requirements

### Requirement 1: Bit cửa

**User Story:** Là người vận hành, tôi muốn YOLO chạy ngay khi board báo mở cửa, để không bỏ lỡ
người bước vào cabin.

#### Acceptance Criteria

1. Bit 5 của giá trị thô `8472-2` báo mở cửa và đánh thức YOLO ngay.
2. Bit 6 của giá trị thô `8472-2` báo đóng cửa.

### Requirement 2: YOLO rồi SCRFD

**User Story:** Là người vận hành, tôi muốn SCRFD chỉ chạy khi YOLO đã xác nhận có người, để không
tốn NPU cho face detection khi cabin không có ai.

#### Acceptance Criteria

1. YOLO xác nhận person track thì cho chạy SCRFD.
2. Recognition vẫn theo từng track.

### Requirement 3: Ngừng inference khi cabin trống

**User Story:** Là người vận hành, tôi muốn các model ngừng inference khi cửa đã đóng và cabin trống,
để NPU nghỉ lúc thang máy idle.

#### Acceptance Criteria

1. Sau khi cửa đóng, hết person track và qua empty grace, các model ngừng inference.

### Requirement 4: Gói partial và mất dữ liệu

**User Story:** Là người vận hành, tôi muốn hệ thống không tin dữ liệu cửa thiếu hoặc cũ, để mất kết
nối không làm tắt nhận diện.

#### Acceptance Criteria

1. Gói partial không có `8472-2` giữ giá trị cửa cũ.
2. Mất dữ liệu hoặc reconnect thì quét YOLO dự phòng; không coi trạng thái cũ là mới.

### Requirement 5: Baseline khi thiếu Socket.IO hoặc person model

**User Story:** Là developer, tôi muốn app chạy pipeline baseline khi không có Socket.IO hoặc person
model, để vẫn dùng được chế độ dev.

#### Acceptance Criteria

1. Khi không có Socket.IO hoặc person model, dùng pipeline baseline để giữ chế độ dev.

### Requirement 6: Rollback

**User Story:** Là người vận hành, tôi muốn tắt tính năng bằng một cờ CLI, để quay về pipeline
baseline nhanh khi có sự cố.

#### Acceptance Criteria

1. Có rollback về baseline bằng `--no-adaptive-inference`.
