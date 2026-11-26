# AIoT Elevator - Smart Cabin — Brochure / One-pager

> Bản nội dung cho tờ rơi một trang (in hai mặt hoặc một mặt A4). Mọi mục `[CẦN ĐIỀN: ...]` phải được
> điền hoặc xóa trước khi in.

---

## Mặt trước

### AIoT Elevator - Smart Cabin

**Bước vào thang, hệ thống chọn tầng cho bạn.**

AIoT Elevator - Smart Cabin là bộ xử lý AI đặt trong cabin thang máy. Thiết bị nhận diện khuôn mặt
cư dân đã đăng ký qua camera trong cabin và tự động gửi lệnh chọn tầng tới bộ điều khiển thang máy.
Toàn bộ xử lý AI chạy ngay trên thiết bị, không gửi video lên cloud.

`[CẦN ĐIỀN: ảnh sản phẩm / ảnh cabin lắp đặt]`

### Lợi ích chính

- **Không cần bấm nút**: cư dân bước vào, hệ thống tự chọn tầng chính hoặc tầng phụ (ví dụ tầng hầm)
  theo chiều chạy của thang.
- **Nút bấm vẫn hoạt động**: hệ thống chỉ thêm tầng được chọn, không thay thế bảng nút.
- **Dữ liệu vận hành thang máy**: tầng đi – tầng đến theo điểm dừng, lưu lượng theo giờ,
  thời gian chờ tại sảnh; xuất file CSV.
- **Dữ liệu khuôn mặt ở lại tòa nhà**: nhận diện, so khớp và lưu dữ liệu khuôn mặt trên thiết bị tại chỗ.
- **Thêm cư dân không gián đoạn**: đăng ký qua web, có hiệu lực ngay mà không cần khởi động lại.

### Tính năng

| Tính năng | Mô tả |
|---|---|
| Gọi tầng tự động | Mỗi cư dân một lần mỗi lượt đi; nhiều người cùng tầng chỉ tạo một lệnh |
| Tầng chính + tầng phụ | Chọn tầng theo tầng hiện tại và chiều chạy của thang |
| Nhật ký lưu lượng | Điểm dừng, chuyến cabin, gọi tầng trong cabin và ở sảnh |
| Đếm người, chuyến từng hành khách | Đang hoàn thiện, chưa kiểm chứng thực địa |
| Cảnh báo vật nuôi | Phát hiện chó/mèo trong cabin, ghi cảnh báo kèm ảnh chụp |
| Ghi video sự kiện | Tùy chọn, tự ghi khi có người trong cabin |
| Tiết kiệm tài nguyên | Mô hình AI chạy theo tín hiệu cửa, tạm ngừng khi cabin trống và cửa đóng |
| Giám sát thiết bị | Ghi định kỳ CPU, RAM, ổ đĩa, nhiệt độ CPU/NPU |

---

## Mặt sau

### Cách hoạt động

1. Cửa mở → thiết bị tìm người trong cabin.
2. Nhận diện khuôn mặt, so khớp với danh sách cư dân trên thiết bị.
3. Xác nhận khi khớp ổn định qua nhiều khung hình liên tiếp.
4. Gửi lệnh chọn tầng qua IoT Core tới bộ điều khiển thang máy.

### Thông số nổi bật

| Hạng mục | Thông số |
|---|---|
| Phần cứng xử lý | Orange Pi A733, NPU tích hợp |
| Mô hình AI | YOLOv5s (người, chó/mèo), SCRFD 2.5G (khuôn mặt), MobileFaceNet (đặc trưng 512 chiều) |
| Camera | Camera IP qua RTSP/HTTP |
| Tốc độ xử lý (đo trên webcam USB) | ~25 FPS phát hiện + nhận diện khuôn mặt; ~15 FPS khi bật thêm phát hiện người và theo dõi |
| Bộ điều khiển thang máy đã tích hợp | INVT (qua IoT Core). Hãng khác: `[CẦN ĐIỀN]` |
| Số cư dân tối đa | `[CẦN ĐIỀN]` |
| Độ chính xác nhận diện | `[CẦN ĐIỀN]` |

Chi tiết xem tài liệu **Thông số kỹ thuật (Datasheet)**.

### Quyền riêng tư

- Lưu đặc trưng khuôn mặt dạng vector số; nhật ký hệ thống chỉ ghi mã số, không ghi tên hay ảnh.
- Nhận diện lại khách vãng lai và lưu ảnh khuôn mặt khách: tùy chọn, bật hay tắt theo từng công trình.
- Dữ liệu lưu lượng, dữ liệu khách vãng lai, ảnh theo chuyến và nhật ký hệ thống: thời hạn lưu cấu hình
  được, tự động xóa khi hết hạn. Dữ liệu cư dân và nhật ký nhận diện: giữ đến khi xóa thủ công.
- Hỗ trợ đơn vị vận hành thực hiện nghĩa vụ theo Luật Bảo vệ dữ liệu cá nhân số 91/2025/QH15.

### Phù hợp với

Chung cư, tòa văn phòng, khách sạn, nhà máy, trường học, bệnh viện và các tòa nhà có người sử dụng
thang máy thường xuyên.

### Liên hệ

**Công ty DAT Group**
Website: www.datgroup.com.vn
Hotline: 18006567
Email: `[CẦN ĐIỀN]`
Địa chỉ: `[CẦN ĐIỀN]`
`[CẦN ĐIỀN: mã QR tới website]`
