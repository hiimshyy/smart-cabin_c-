# Thiết kế

`SocketIoClient` gửi `CONN_SUBSCRIBE` sau namespace ready, lọc `CONN_DATA` đúng SN/level và
khóa `8472-2`. Callback đưa sample thô vào atomic state; main đọc tại frame boundary.

`DoorInferenceScheduler` thuần logic dùng bit cửa, monotonic time và TrackSummary để quyết định
YOLO/SCRFD. Sau YOLO, main tính lại summary để SCRFD có thể chạy cùng frame khi track vừa được
xác nhận. Recognition/interaction dùng lịch retry và confirm hiện có; track đã confirmed không
cần SCRFD nữa. Context NPU giữ nguyên; skipped stage không gọi `awnn_run` hoặc decode.

Mặc định: cửa mở YOLO 15 FPS, occupied YOLO 5 FPS (10 nếu unresolved), face 10 FPS,
degraded YOLO 2 FPS khi trống (10 nếu unresolved), empty grace 3 giây, TTL 5 giây.
Các con số cần đo và chỉnh trên camera/board thật.
