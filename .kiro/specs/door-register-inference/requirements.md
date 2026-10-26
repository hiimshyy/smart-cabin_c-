# Điều phối inference theo thanh ghi cửa 8472-2

Quyết định 2026-10-22: lấy dữ liệu từ `CONN_DATA` qua Socket.IO của iot_core theo
`/edge/native_core/socket_test.cpp`, slave id 2. Model vẫn load một lần tại startup.

- Bit 5 của giá trị thô `8472-2` báo mở cửa và đánh thức YOLO ngay. Bit 6 báo đóng cửa.
- YOLO xác nhận person track thì cho chạy SCRFD; recognition vẫn theo từng track.
- Sau khi cửa đóng, hết person track và qua empty grace, các model ngừng inference.
- Gói partial không có `8472-2` giữ giá trị cửa cũ. Mất dữ liệu hoặc reconnect thì quét YOLO
  dự phòng; không coi trạng thái cũ là mới.
- Khi không có Socket.IO hoặc person model, dùng pipeline baseline để giữ chế độ dev.
- Có rollback về baseline bằng `--no-adaptive-inference`.

Đợt này chưa thay đổi chính sách gửi lệnh gọi tầng. Cần smoke test board/camera thật trước deploy.
