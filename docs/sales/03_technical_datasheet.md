# AIoT Elevator - Smart Cabin — Thông số kỹ thuật (Datasheet)

**Nhà cung cấp:** Công ty DAT Group · www.datgroup.com.vn · Hotline 18006567
**Phiên bản tài liệu:** `[CẦN ĐIỀN: số phiên bản / ngày phát hành]`
**Phiên bản phần mềm:** `[CẦN ĐIỀN: số phiên bản phần mềm]`

> Các giá trị trong tài liệu là giá trị mặc định của phần mềm hiện tại, trừ khi ghi khác. Mục
> `[CẦN ĐIỀN: ...]` là thông tin chưa được đo hoặc chưa được xác nhận, phải điền trước khi gửi khách hàng.

---

## 1. Tổng quan

AIoT Elevator - Smart Cabin là thiết bị AI biên (edge) đặt trong cabin thang máy. Thiết bị đọc luồng
video từ camera IP trong cabin, nhận diện khuôn mặt cư dân đã đăng ký và gửi lệnh chọn tầng tới bộ
điều khiển thang máy thông qua IoT Core. Thiết bị đồng thời ghi nhật ký lưu lượng thang máy. Đếm người
vào/ra cabin đang hoàn thiện (xem mục 8). Mọi xử lý AI chạy cục bộ trên NPU của thiết bị.

Mỗi tiến trình phần mềm phục vụ một cabin.

---

## 2. Phần cứng

| Hạng mục | Thông số |
|---|---|
| Bo mạch xử lý | Orange Pi A733 |
| Bộ tăng tốc AI | NPU tích hợp (1 lõi NPU dùng chung cho các mô hình) |
| Hiệu năng NPU (TOPS) | `[CẦN ĐIỀN]` |
| CPU / RAM / bộ nhớ lưu trữ | `[CẦN ĐIỀN]` |
| Hệ điều hành | Linux trên Orange Pi `[CẦN ĐIỀN: bản phân phối và phiên bản]` |
| Nguồn điện, công suất tiêu thụ | `[CẦN ĐIỀN]` |
| Kích thước, vỏ, cách lắp | `[CẦN ĐIỀN]` |
| Nhiệt độ / độ ẩm hoạt động | `[CẦN ĐIỀN]` |
| Chứng nhận (CE, FCC, hợp quy...) | `[CẦN ĐIỀN]` |

### Camera

| Hạng mục | Thông số |
|---|---|
| Loại camera vận hành | Camera IP, nguồn RTSP / HTTP / HTTPS |
| Số camera mỗi cabin | 1 (phiên bản hiện tại dùng camera đầu tiên trong danh sách cấu hình) |
| Camera USB | Hỗ trợ cho trình diễn và phát triển, không dùng cho vận hành |
| Bộ đệm RTSP | 100 ms mặc định, cấu hình được |
| Tự kết nối lại khi mất tín hiệu | Có, thời gian chờ tăng dần từ 0,5 s đến 10 s (cấu hình được) |
| Chuẩn nén video đã kiểm chứng | `[CẦN ĐIỀN: H.264 / H.265 đã thử nghiệm]` |
| Độ phân giải, vị trí và góc lắp khuyến nghị | `[CẦN ĐIỀN]` |

---

## 3. Mô hình AI

| Chức năng | Mô hình | Đầu vào | Ghi chú |
|---|---|---|---|
| Phát hiện người, chó, mèo | YOLOv5s (COCO 80 lớp), lượng tử hóa uint8 cho NPU | 640×640 | Chó/mèo lấy từ cùng lượt chạy, không tốn thêm lượt NPU |
| Phát hiện khuôn mặt + 5 điểm mốc | SCRFD 2.5G (bnkps), uint8 | 640×640 | Kích thước mô hình ~947 KB |
| Trích đặc trưng khuôn mặt | MobileFaceNet (w600k_mbf), uint8 | 112×112 (căn chỉnh theo 5 điểm mốc) | Vector 512 chiều, chuẩn hóa L2; mô hình ~2,7 MB |
| Theo dõi người | ByteTrack (mặc định khi vận hành) | — | Giữ danh tính khi người quay lưng hoặc mất khuôn mặt tạm thời |
| Giấy phép sử dụng thương mại các mô hình | `[CẦN ĐIỀN: xác nhận giấy phép thương mại của từng mô hình]` | | |

### Nhận diện và xác nhận danh tính

| Tham số | Mặc định | Phạm vi |
|---|---|---|
| Phương pháp so khớp | Độ tương đồng cosine, lấy giá trị cao nhất trên mọi mẫu của mỗi cư dân | — |
| Ngưỡng nhận diện | 0,35 | 0,05 – 0,95 |
| Số khung hình liên tiếp để xác nhận | 5 | ≥ 1 |
| Thời gian chờ giữa hai lần xác nhận cùng người | 3 000 ms | ≥ 0 |
| Thời gian để kết luận "người lạ" | 2 000 ms | ≥ 0 |
| Chống giả mạo bằng ảnh/video (liveness) | Chưa có trong phiên bản hiện tại | — |

---

## 4. Hiệu năng

Số liệu đo nội bộ trên webcam USB:

| Chỉ số | Giá trị |
|---|---|
| Tốc độ toàn luồng (phát hiện + nhận diện khuôn mặt) | ~25 FPS |
| Tốc độ khi bật thêm phát hiện người và theo dõi | ~15 FPS |
| Thời gian phát hiện khuôn mặt trên NPU | ~10–17 ms |
| Thời gian nhận diện trên NPU | ~3 ms / khuôn mặt |
| Thời gian đăng ký một ảnh (sau lần khởi tạo NPU đầu tiên) | ~20–30 ms |

Chưa đo hoặc chưa xác nhận:

| Chỉ số | Giá trị |
|---|---|
| FPS và độ trễ trên camera RTSP tại công trình | `[CẦN ĐIỀN]` |
| Thời gian từ lúc bước vào cabin đến khi gửi lệnh chọn tầng | `[CẦN ĐIỀN]` |
| Tỉ lệ nhận nhầm (FAR) / tỉ lệ bỏ sót (FRR) | `[CẦN ĐIỀN]` |
| Số cư dân tối đa đã kiểm chứng | `[CẦN ĐIỀN]` |
| Độ chính xác đếm người | `[CẦN ĐIỀN]` |

Dung lượng lưu một mẫu đặc trưng khuôn mặt: 2 KB (512 số thực 32 bit).

---

## 5. Gọi tầng tự động

| Hạng mục | Thông số |
|---|---|
| Kênh điều khiển | Socket.IO tới IoT Core; IoT Core ghi lệnh xuống bộ điều khiển thang máy |
| Bộ điều khiển đã tích hợp | INVT (theo bảng thanh ghi của bộ điều khiển) |
| Bộ điều khiển khác | `[CẦN ĐIỀN]` |
| Giao tiếp vật lý IoT Core ↔ bộ điều khiển | `[CẦN ĐIỀN]` |
| Tầng của cư dân | Tầng chính và tầng phụ (tùy chọn); hỗ trợ tầng hầm B |
| Chọn tầng đích | Theo tầng hiện tại và chiều chạy, đọc từ thanh ghi trạng thái và gọi tầng ở sảnh |
| Dải tầng | Cấu hình theo cabin (tầng thấp nhất / cao nhất) |
| Số tầng tối đa hỗ trợ | `[CẦN ĐIỀN]` |
| Chống gọi trùng | Mỗi cư dân tối đa 1 lần mỗi lượt đi thang; mỗi tầng tối đa 1 lệnh mỗi lượt |
| Kết thúc một lượt đi thang | Cabin không có người liên tục 5 s (mặc định) |
| Thứ tự gửi | Lần lượt theo thời điểm xác nhận, tối đa 1 lệnh chờ phản hồi tại một thời điểm |
| Thời hạn lệnh trước khi gửi | 5 s; quá hạn thì hủy, không gửi bù |
| Thời gian chờ phản hồi | 3 s; không phản hồi thì ghi nhận "không rõ kết quả", không tự gửi lại |
| Số lệnh tối đa mỗi lượt đi thang | 16 |
| Không gọi tầng khi | Người lạ; cư dân chưa khai báo tầng; tầng ngoài dải cấu hình; cabin đang ở đúng tầng đó; thang đang chạy ngược chiều với tầng cần đến (cư dân không được xét lại cho đến hết lượt đi thang) |
| Nút bấm trong cabin | Không bị ảnh hưởng; lệnh của hệ thống chỉ thêm tầng được chọn |

---

## 6. Điều phối AI theo tín hiệu cửa

| Hạng mục | Hoạt động |
|---|---|
| Cửa mở | Bắt đầu phát hiện người ngay |
| Có người | Chạy phát hiện khuôn mặt; nhận diện theo từng người được theo dõi |
| Cửa đóng và cabin trống | Sau thời gian chờ, các mô hình ngừng chạy (vẫn nạp sẵn trong bộ nhớ) |
| Mất tín hiệu cửa | Quét phát hiện người ở tần suất thấp để không bỏ sót |
| Tắt tính năng | Có thể quay về chạy liên tục |

---

## 7. Nhật ký lưu lượng thang máy (tùy chọn)

| Hạng mục | Thông số |
|---|---|
| Dữ liệu ghi | Điểm dừng, chuyến cabin, gọi tầng trong cabin, gọi tầng ở sảnh (từ thanh ghi bộ điều khiển) |
| Chỉ số khai thác | Ma trận tầng đi – tầng đến theo điểm dừng, lưu lượng theo giờ/chiều, thời gian chuyến, thời gian chờ tại sảnh |
| Chuyến từng hành khách, tải cabin | Đang hoàn thiện, chưa kiểm chứng thực địa (dựa trên bộ đếm người ở mục 8 và danh tính từ nhận diện) |
| Nguồn dữ liệu | Thanh ghi bộ điều khiển thang máy; bộ đếm người và nhận diện cho dữ liệu từng hành khách |
| Gắn cờ chế độ đặc biệt | Bảo trì, lỗi, khóa, báo cháy, độc lập, quá tải, mất dữ liệu thanh ghi |
| Sai số thời gian | Khoảng một chu kỳ gửi dữ liệu của gateway (đo được trung vị ~5,6 s) |
| Lưu trữ | Cơ sở dữ liệu SQLite riêng trên thiết bị, tách khỏi dữ liệu cư dân |
| Xuất dữ liệu | File CSV theo khoảng ngày, qua công cụ quản trị trên thiết bị |
| Giao diện báo cáo web | Chưa có trong phiên bản hiện tại |
| Sức chứa cabin dùng để kiểm tra số đếm | 15 người (cấu hình được) |

---

## 8. Đếm người trong cabin (đang hoàn thiện)

Tính năng đã có trong phần mềm nhưng chưa hoàn tất thử nghiệm nhiều người, thử nghiệm dài hạn và
kiểm chứng tại công trình. Không dùng số đếm này cho cam kết với khách hàng.

| Hạng mục | Thông số |
|---|---|
| Phương pháp | Đếm lượt đi qua vùng cửa (polygon cấu hình theo từng camera) |
| Mức tin cậy | "Chắc chắn" / "Không chắc chắn"; chuyển "Không chắc chắn" khi mất camera, mất dấu người trong vùng cửa, đổi ID không rõ |
| Lưu trạng thái | Lưu xuống đĩa, khôi phục sau khi khởi động lại |
| Giới hạn | Vùng cửa phải nằm trọn trong khung hình; nhiều người chồng lấn có thể bị đếm thiếu |

---

## 9. Tính năng khác

| Tính năng | Thông số |
|---|---|
| Cảnh báo chó/mèo | Ngưỡng tin cậy 0,8 mặc định; giãn cách 3 s mỗi loài; ghi cảnh báo và ảnh chụp có khung trên thiết bị. Gửi cảnh báo ra hệ thống ngoài: chưa có |
| Ghi video theo sự kiện (mặc định tắt) | Ghi khi có người; gồm 1 s trước lúc phát hiện; dừng sau khi cabin trống 3 s; 15 FPS; chiều rộng 960 px; ghi trên luồng riêng |
| Giám sát thiết bị | Mỗi 30 s ghi CPU, RAM, ổ đĩa, nhiệt độ CPU/NPU, trạng thái đếm người và nhật ký lưu lượng |
| Nhật ký hệ thống | File theo ngày, giữ 14 ngày (mặc định); không chứa thông tin cá nhân |
| Màn hình xem trực tiếp | Tùy chọn, hiển thị khung người, khung mặt, tên chào và tầng (font không dấu); vận hành thường chạy không màn hình |

---

## 10. Đăng ký và quản trị cư dân

| Hạng mục | Thông số |
|---|---|
| Kênh đăng ký | API REST trên thiết bị (HTTP, chưa có xác thực), phục vụ giao diện quản trị web; công cụ dòng lệnh trên thiết bị |
| Ảnh đăng ký | JPG, PNG, BMP, WEBP; nhiều ảnh mỗi người; mỗi ảnh tạo một mẫu đặc trưng |
| Thông tin cư dân | Mã cư dân, họ tên, tầng chính, tầng phụ, vai trò, tên chào, căn hộ, ngôn ngữ |
| Hiệu lực sau đăng ký | Không cần khởi động lại; thiết bị kiểm tra thay đổi mỗi 1 s (cấu hình được) |
| Tính toàn vẹn | Ghi đăng ký theo giao dịch nguyên tử: không có trạng thái "đăng ký dở dang" |
| Xóa cư dân | Xóa hồ sơ, thư mục ảnh và toàn bộ mẫu đặc trưng của cư dân |
| Khách vãng lai | Tùy chọn nhận diện lại qua nhiều chuyến (bật/tắt theo từng công trình); quản trị viên xem, gộp, xóa hoặc chuyển thành cư dân |
| Camera của cabin | Chọn trong phần cấu hình camera trên web; có hiệu lực sau khi khởi động lại dịch vụ |
| Ngưỡng nhận diện | Tham số cài đặt trên từng thiết bị; có hiệu lực sau khi khởi động lại dịch vụ |
| Tham số khác theo cabin | Số khung hình xác nhận, thời gian chờ, kết nối lại, dải tầng; có hiệu lực sau khi khởi động lại dịch vụ |

---

## 11. Dữ liệu, bảo mật và quyền riêng tư

| Hạng mục | Thông số |
|---|---|
| Nơi xử lý AI | Trên thiết bị, không gửi video lên cloud |
| Dữ liệu cư dân lưu trên thiết bị | Hồ sơ cư dân, ảnh đăng ký, mẫu đặc trưng 512 chiều, nhật ký nhận diện (mã cư dân, điểm tương đồng, thời gian) |
| Hồ sơ cư dân trên nền tảng quản trị (IoT Core) | Vị trí lưu trữ: `[CẦN ĐIỀN]` |
| Nội dung nhật ký hệ thống | Chỉ mã số (mã cư dân, mã khách, mã chuyến, tầng); không có tên, tên chào, căn hộ, vector, đường dẫn ảnh |
| Thông tin đăng nhập camera | Được ẩn trong nhật ký |
| Quyền truy cập cơ sở dữ liệu lưu lượng | Chỉ tài khoản dịch vụ (quyền file 0600) |
| Nhận diện lại khách vãng lai | Tùy chọn, bật/tắt theo từng công trình. Khi bật: lưu mẫu đặc trưng 512 chiều của người chưa đăng ký, gắn mã số ẩn danh. Trạng thái trong cấu hình bàn giao: `[CẦN ĐIỀN]` |
| Lưu ảnh khuôn mặt theo chuyến | Tùy chọn: tắt / chỉ khách / tất cả; ảnh 112×112 JPEG. Trạng thái trong cấu hình bàn giao: `[CẦN ĐIỀN]` |
| Thời hạn giữ dữ liệu lưu lượng | 730 ngày (cấu hình được), gồm cả lịch sử chuyến gắn mã cư dân |
| Thời hạn giữ mẫu khuôn mặt khách vãng lai | 90 ngày từ lần thấy cuối (cấu hình được) |
| Thời hạn giữ ảnh khuôn mặt theo chuyến | 30 ngày, tối đa 5 GB (cấu hình được) |
| Dọn dữ liệu hết hạn | Tự động mỗi giờ cho dữ liệu lưu lượng, mẫu khách vãng lai và ảnh theo chuyến |
| Bảo vệ ổ đĩa | Ngừng lưu ảnh khi còn dưới 4 096 MB trống; ngừng ghi bản ghi lưu lượng khi dưới 1 024 MB |
| Thời hạn giữ nhật ký nhận diện cư dân | Phiên bản hiện tại chưa có xóa tự động; giữ đến khi xóa thủ công. `[CẦN ĐIỀN: chính sách và cách xóa định kỳ]` |
| Mã hóa dữ liệu lưu trữ | `[CẦN ĐIỀN]` |
| Xác thực truy cập API quản trị trên thiết bị | Chưa có trong phiên bản hiện tại; chỉ đặt thiết bị trong mạng nội bộ được kiểm soát |
| Mã hóa kết nối API quản trị trên thiết bị | Chưa có (HTTP) trong phiên bản hiện tại |

---

## 12. Độ tin cậy vận hành

- Chạy như dịch vụ hệ thống, tự khởi động lại sau 5 s khi tiến trình dừng.
- Ghi mạng, ghi đĩa, ghi video và ghi cơ sở dữ liệu chạy trên luồng riêng với hàng đợi giới hạn,
  không chặn luồng xử lý camera.
- Lệnh gọi tầng gửi bất đồng bộ; lỗi một lệnh không ảnh hưởng nhận diện và các lệnh khác.
- Lỗi khi nạp lại danh sách cư dân: tiếp tục dùng danh sách cũ và thử lại.

---

## 13. Giới hạn đã biết của phiên bản hiện tại

- Chưa có chống giả mạo bằng ảnh in hoặc video (liveness).
- Một camera mỗi cabin.
- Đổi camera hoặc tham số AI cần khởi động lại dịch vụ (gián đoạn vài giây).
- Cảnh báo chó/mèo chỉ ghi trên thiết bị.
- Nhật ký lưu lượng xuất CSV qua công cụ trên thiết bị, chưa có báo cáo web.
- Đếm người và dữ liệu chuyến từng hành khách chưa được kiểm chứng thực địa. Số đếm có thể lệch khi
  camera không thấy hết cửa hoặc người chồng lấn.
- API quản trị trên thiết bị chưa có xác thực và chưa mã hóa kết nối.
- Nhật ký nhận diện cư dân chưa có xóa tự động.
- NPU một lõi dùng chung: khi đang đăng ký khuôn mặt, tốc độ xử lý có thể chững lại trong thời gian ngắn.

---

## 14. Yêu cầu lắp đặt

| Hạng mục | Yêu cầu |
|---|---|
| Camera | 1 camera IP trong cabin, nhìn thấy vùng cửa và khuôn mặt người bước vào |
| Mạng | Thiết bị AI kết nối được tới camera và IoT Core `[CẦN ĐIỀN: yêu cầu mạng cụ thể]` |
| Kết nối thang máy | IoT Core kết nối bộ điều khiển thang máy `[CẦN ĐIỀN: phần cứng gateway và cách đấu nối]` |
| Nguồn điện trong cabin | `[CẦN ĐIỀN]` |
| Bảo hành, bảo trì | `[CẦN ĐIỀN]` |
