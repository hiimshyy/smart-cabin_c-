# Đếm ngườị trong cabin bằng Counting Zone

## Bối cảnh

Camera cabin có góc nhìn hạn chế và góc chết, không bao phủ toàn bộ cabin. Không thể dùng tổng số ngườị detect trong một frame làm số ngườị hiện tại. Thay vào đó, hệ thống đếm từng ngườị khi họ đi qua vùng cửa thang máy.

## Yêu cầu

1. **Counting zone**:
   - Cấu hình vùng đếm là polygon trên một file JSON riêng cho từng camera.
   - File JSON có version, lưu tọa độ chuẩn hóa `[0,1]` cùng kích thước frame gốc (`reference_width/height`). Runtime chuyển sang pixel.
   - Validate khi load: version hợp lệ, polygon ≥ 3 điểm, tọa độ trong `[0,1]`, hướng vào cabin hợp lệ.
   - Ghi file config an toàn: không ghi đè file hợp lệ nếu lưu thất bại.

2. **Đếm event-based**:
   - Dùng điểm dưới chân (bottom-center bbox) của mỗi person track làm điểm tham chiếu.
   - Phát hiện entry khi track đi từ ngoài → trong zone → vào trong cabin đủ displacement.
   - Phát hiện exit khi track đi từ trong cabin → trong zone → ra ngoài.
   - Không đếm khi track vào zone rồi lùi lại cùng phía (aborted crossing).
   - Không dùng door register `8472-2` để chứng minh cabin trống.

3. **Chống drift và xử lý bất định**:
   - Không tự reset occupancy khi mất track, timeout, hay cửa đóng.
   - Giữ occupancy cuối cùng; đánh dấu `Uncertain` khi RTSP outage, tracking chỉ còn prediction, track lost trong zone, track ID churn không rõ, hoặc crossing bị abort.
   - `Certain` trở lại khi có entry/exit validated hoặc reconcile.
   - Xử lý track-ID churn bằng ghost state ngắn hạn để tránh double count.

4. **Persistence**:
   - Lưu occupancy (`count`, `confidence`, `generation`, `saved_at_ms`) ra file JSON nhỏ.
   - Ghi atomic (temp → fsync → rename).
   - Khởi động lại app phải phục hồi occupancy; nếu không có file hoặc file corrupt thì `count=0, confidence=Uncertain` và log warning.

5. **Tích hợp**:
   - CLI `--occupancy-config PATH` và `--occupancy-state PATH`.
   - `env.sh` tạo đường dẫn mặc định theo `FACE_CABIN_ID`.
   - Hiển thị occupancy trên preview HUD và log `sys` metrics.
   - Cung cấp API `reconcile(count)` / `reconcile_empty()` cho nguồn bên ngoài trong tương lai.

6. **Tests**:
   - Load/save/validate config.
   - Persistence corruption và failed-save không clobber.
   - Entry/exit, duplicate, aborted, track loss, RTSP outage, ID churn, door degraded, reconcile empty.

## Known limitations

- Zone phải phủ hết cửa camera nhìn thấy; nếu camera không thấy một phần cửa thì count vẫn có thể drift và chuyển Uncertain.
- Nhiều ngườị chồng lấn thành một track sẽ bị đếm thiếu — cần reconcile ngoài.
