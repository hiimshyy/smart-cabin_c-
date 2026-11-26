# AIoT Elevator - Smart Cabin — Pitch deck (dàn ý từng slide)

> Tài liệu nội bộ dùng để dựng slide (PowerPoint/Canva) khi gặp khách hàng và tại triển lãm.
> Mỗi slide gồm: **Tiêu đề**, **Nội dung trên slide**, **Hình ảnh gợi ý**, **Ghi chú người thuyết trình**.
> Mọi mục `[CẦN ĐIỀN: ...]` phải được điền hoặc xóa trước khi trình chiếu.
> Không thêm giá, kết quả thí điểm, tên khách hàng hoặc so sánh đối thủ khi chưa có số liệu xác nhận.

---

## Slide 1 — Trang bìa

**Nội dung trên slide**

- **AIoT Elevator - Smart Cabin**
- Thang máy nhận diện cư dân và tự gọi tầng, xử lý AI ngay trong cabin
- Công ty DAT Group · www.datgroup.com.vn · Hotline 18006567

**Hình ảnh gợi ý:** ảnh cabin thang máy có camera, logo DAT Group. `[CẦN ĐIỀN: ảnh sản phẩm thật / logo]`

**Ghi chú người thuyết trình:** Giới thiệu ngắn: đây là bộ xử lý AI gắn trong cabin, nhìn qua camera,
nhận ra cư dân đã đăng ký và gửi lệnh chọn tầng tới bộ điều khiển thang máy.

---

## Slide 2 — Bài toán của tòa nhà

**Nội dung trên slide**

- Cư dân phải bấm nút chọn tầng mỗi lần đi thang, kể cả khi tay đang bận đồ.
- Ban quản lý ít có dữ liệu về cách thang máy được sử dụng: đi từ tầng nào đến tầng nào, giờ nào đông,
  chờ ở sảnh bao lâu.
- Camera trong cabin thường chỉ dùng để ghi hình, chưa khai thác được thông tin.

**Hình ảnh gợi ý:** ba biểu tượng: bàn tay bấm nút, biểu đồ trống, camera.

**Ghi chú người thuyết trình:** Hỏi khách về tình trạng hiện tại của tòa nhà họ. Không đưa số liệu
thống kê thị trường nếu chưa có nguồn. `[CẦN ĐIỀN: số liệu thị trường có trích nguồn, nếu muốn dùng]`

---

## Slide 3 — Giải pháp

**Nội dung trên slide**

- Một thiết bị AI biên (edge) đặt trong cabin, đọc luồng video từ camera IP.
- Nhận diện khuôn mặt cư dân đã đăng ký → tự động gửi lệnh chọn tầng của cư dân đó.
- Đồng thời thu thập dữ liệu lưu lượng thang máy từ bộ điều khiển (điểm dừng, gọi tầng, thời gian chờ).
- Toàn bộ xử lý AI chạy trên thiết bị, không gửi video lên cloud.
- Nút bấm của thang máy vẫn hoạt động bình thường.

**Hình ảnh gợi ý:** sơ đồ đơn giản: Camera → Thiết bị AI → Bộ điều khiển thang máy.

**Ghi chú người thuyết trình:** Nhấn mạnh hệ thống chỉ *thêm* tầng được chọn, không khóa hay thay thế
nút bấm. Đây là tiện ích chọn tầng, không phải hệ thống kiểm soát ra vào.

---

## Slide 4 — Cách hoạt động

**Nội dung trên slide**

1. Cửa cabin mở → thiết bị bắt đầu tìm người trong khung hình.
2. Phát hiện người → phát hiện khuôn mặt → trích đặc trưng khuôn mặt (vector 512 chiều).
3. So khớp với cơ sở dữ liệu cư dân trên thiết bị; chỉ xác nhận khi khớp ổn định qua nhiều khung hình liên tiếp.
4. Chọn tầng đích của cư dân theo vị trí và chiều chạy hiện tại của thang.
5. Gửi lệnh chọn tầng qua IoT Core tới bộ điều khiển thang máy; ghi nhận kết quả phản hồi.
6. Cửa đóng và cabin trống → các mô hình AI tạm ngừng chạy.

**Hình ảnh gợi ý:** sơ đồ 6 bước theo hàng ngang.

**Ghi chú người thuyết trình:** Bước 3 dùng cơ chế xác nhận: mặc định cần 5 khung hình liên tiếp khớp
trên ngưỡng mới coi là nhận ra, để tránh nhận nhầm do một khung hình. Bước 6 dựa trên tín hiệu cửa
đọc từ bộ điều khiển; nếu mất tín hiệu cửa, thiết bị vẫn quét người ở tần suất thấp.

---

## Slide 5 — Gọi tầng tự động thông minh

**Nội dung trên slide**

- Mỗi cư dân có **tầng chính** và có thể thêm **tầng phụ** (ví dụ tầng hầm B).
- Tự chọn tầng phù hợp theo tầng hiện tại và chiều chạy của thang.
  Ví dụ: cư dân tầng 7 có tầng phụ B → vào thang ở tầng 7 khi đi xuống thì gọi B; vào ở B thì gọi 7.
- Trong một lượt đi thang:
  - mỗi cư dân chỉ được gọi tầng một lần, kể cả khi quay mặt đi rồi quay lại;
  - nhiều cư dân cùng tầng chỉ tạo một lệnh;
  - nhiều tầng khác nhau được gửi lần lượt, chờ phản hồi từng lệnh.
- Người lạ hoặc cư dân chưa khai báo tầng: không gọi tầng.
- Thang đang chạy ngược chiều với tầng cần đến: không gọi tầng trong lượt đi đó, cư dân bấm nút.

**Hình ảnh gợi ý:** minh họa 2 cư dân cùng tầng → 1 lệnh; 2 cư dân khác tầng → 2 lệnh.

**Ghi chú người thuyết trình:** Ví dụ trường hợp ngược chiều: cư dân chỉ có tầng 7, vào thang ở tầng 3
khi thang đang đi xuống. Hệ thống không gọi tầng 7 và không xét lại cư dân này cho đến khi kết thúc
lượt đi thang (cabin trống 5 giây). Lệnh gọi tầng gửi bất đồng bộ nên không làm chậm luồng xử lý camera.
Nếu lệnh hết hạn trước khi gửi được (mặc định 5 giây) thì bị hủy, không gửi lệnh cũ khi kết nối phục hồi.

---

## Slide 6 — Dữ liệu lưu lượng thang máy

**Nội dung trên slide**

- Ghi lại tự động từ bộ điều khiển thang máy: các điểm dừng, các chuyến cabin, các lần bấm gọi tầng
  trong cabin và gọi tầng ở sảnh.
- Từ đó có: ma trận tầng đi – tầng đến theo điểm dừng, lưu lượng theo giờ và theo chiều,
  thời gian chuyến, thời gian chờ tại sảnh.
- Gắn cờ khi thang ở chế độ bảo trì, lỗi, khóa, báo cháy, độc lập hoặc quá tải.
- Lưu cục bộ trên thiết bị, xuất file CSV theo khoảng ngày.
- Đang hoàn thiện (chưa kiểm chứng thực địa): chuyến đi của từng hành khách và số người trong cabin.

**Hình ảnh gợi ý:** biểu đồ cột lưu lượng theo giờ, ma trận tầng. `[CẦN ĐIỀN: biểu đồ mẫu dựng từ dữ liệu thật đã ẩn danh, nếu có]`

**Ghi chú người thuyết trình:** Dữ liệu điểm dừng và gọi tầng lấy trực tiếp từ bộ điều khiển thang máy,
không phụ thuộc camera. Thời gian chờ có sai số khoảng một chu kỳ gửi dữ liệu của gateway (đo được
khoảng 5,6 giây). Chuyến đi từng hành khách và số người trong cabin dựa trên bộ đếm người, bộ đếm này
chưa qua thử nghiệm dài hạn tại công trình nên không trình bày như tính năng đã hoàn thiện.
Phiên bản hiện tại xuất dữ liệu qua công cụ trên thiết bị, chưa có giao diện báo cáo web.

---

## Slide 7 — Tính năng bổ sung

**Nội dung trên slide**

- **Cảnh báo chó/mèo trong cabin**: ghi cảnh báo và lưu ảnh chụp sự kiện, không tốn thêm lượt chạy AI.
- **Ghi video theo sự kiện** (tùy chọn): tự ghi khi có người, gồm 1 giây trước lúc phát hiện.
- **Giám sát sức khỏe thiết bị**: CPU, RAM, ổ đĩa, nhiệt độ CPU/NPU được ghi định kỳ.

- **Đếm người vào/ra cabin** (đang hoàn thiện, chưa kiểm chứng thực địa): đếm qua vùng cửa,
  có đánh dấu mức tin cậy khi dữ liệu không chắc chắn.

**Hình ảnh gợi ý:** 4 biểu tượng tương ứng.

**Ghi chú người thuyết trình:** Cảnh báo động vật hiện chỉ ghi trên thiết bị, chưa đẩy ra ứng dụng
hay hệ thống ngoài. Ảnh cảnh báo là nguyên khung hình camera nên có thể có hình người trong cabin; ảnh
chỉ dùng cho gỡ lỗi nội bộ và R&D của DAT Group và chưa có xóa tự động. Bộ đếm người chưa qua thử nghiệm tải nhiều người và thử nghiệm dài hạn; không hứa
độ chính xác. Số đếm phụ thuộc góc camera: nếu camera không thấy hết cửa hoặc nhiều người chồng lên
nhau thì số đếm có thể lệch.

---

## Slide 8 — Công nghệ AI tại biên

**Nội dung trên slide**

- Phần cứng: Orange Pi A733 có NPU tích hợp.
- 3 mô hình AI chạy trên NPU:
  - YOLOv5s: phát hiện người (và chó/mèo);
  - SCRFD 2.5G: phát hiện khuôn mặt và 5 điểm mốc;
  - MobileFaceNet: trích đặc trưng khuôn mặt 512 chiều.
- Theo dõi từng người qua nhiều khung hình (ByteTrack), giữ danh tính khi người quay lưng.
- Tốc độ xử lý ở chế độ vận hành (3 mô hình + theo dõi): `[CẦN ĐIỀN: FPS chế độ vận hành]`.

**Hình ảnh gợi ý:** ảnh bo mạch, sơ đồ 3 mô hình.

**Ghi chú người thuyết trình:** Thời gian chạy trên NPU đo nội bộ trên webcam USB, ở chế độ chỉ phát
hiện + nhận diện khuôn mặt (không dùng cho vận hành): phát hiện khuôn mặt ~10–17 ms, nhận diện ~3 ms
mỗi khuôn mặt. Không đưa con số FPS khi mục FPS trên slide chưa được điền. Hiệu năng trên
camera IP thực tế tại công trình:
`[CẦN ĐIỀN: FPS và độ trễ đo trên camera RTSP tại công trình]`. Độ chính xác nhận diện:
`[CẦN ĐIỀN: tỉ lệ nhận nhầm / bỏ sót đo trên bộ dữ liệu thật]`.

---

## Slide 9 — Quyền riêng tư và dữ liệu cá nhân

**Nội dung trên slide**

- Xử lý AI hoàn toàn trên thiết bị trong tòa nhà; không gửi video lên cloud.
- Lưu đặc trưng khuôn mặt dạng vector số để so khớp.
- Nhật ký hệ thống chỉ ghi mã số, không ghi tên, căn hộ, vector hay ảnh.
- Nhận diện lại khách vãng lai và lưu ảnh khuôn mặt khách là **tính năng tùy chọn**, bật hay tắt
  theo từng công trình; chỉ bật sau khi hoàn tất thủ tục pháp lý.
- Dữ liệu lưu lượng, mẫu khuôn mặt khách vãng lai, ảnh khuôn mặt theo chuyến và nhật ký hệ thống
  có thời hạn lưu cấu hình được và tự động xóa khi hết hạn.
- Dữ liệu cư dân, nhật ký nhận diện cư dân, ảnh cảnh báo chó/mèo và video sự kiện (nếu bật) được giữ
  đến khi xóa thủ công.
- Hỗ trợ đơn vị vận hành đáp ứng Luật Bảo vệ dữ liệu cá nhân số 91/2025/QH15.

**Hình ảnh gợi ý:** biểu tượng khiên/ổ khóa, sơ đồ "dữ liệu ở lại tòa nhà".

**Ghi chú người thuyết trình:** Dữ liệu khuôn mặt là dữ liệu cá nhân nhạy cảm. Đơn vị vận hành tòa nhà
cần thông báo tại thang máy và có căn cứ xử lý (ví dụ sự đồng ý của cư dân) trước khi đăng ký khuôn
mặt. Không nói "mặc định tắt" cho nhận diện lại khách vãng lai: phần mềm gốc để tắt, nhưng cấu hình
triển khai mẫu hiện tại đang bật cả nhận diện lại và lưu ảnh khách.
`[CẦN ĐIỀN: xác nhận trạng thái bật/tắt hai tính năng này trong cấu hình bàn giao cho khách hàng]`.
Chi tiết trong tài liệu FAQ. Không cam kết thay khách hàng về tuân thủ pháp lý.

---

## Slide 10 — Đăng ký và quản trị cư dân

**Nội dung trên slide**

- Đăng ký khuôn mặt bằng ảnh qua giao diện quản trị web (API REST trên thiết bị).
- Mỗi cư dân có thể có nhiều ảnh; mỗi ảnh tạo một mẫu đặc trưng, so khớp lấy mẫu giống nhất.
- Người mới được nhận diện ngay sau khi đăng ký, không cần khởi động lại (hệ thống kiểm tra thay đổi mỗi 1 giây).
- Xóa cư dân trên web thì dữ liệu khuôn mặt của cư dân đó trên thiết bị cũng bị xóa.
- Camera của từng cabin chọn trong phần cấu hình camera trên web; ngưỡng nhận diện là tham số cài đặt
  trên từng thiết bị.

**Hình ảnh gợi ý:** `[CẦN ĐIỀN: ảnh chụp màn hình giao diện quản trị web]`

**Ghi chú người thuyết trình:** Đổi camera hoặc tham số AI cần khởi động lại dịch vụ (gián đoạn vài
giây). Thêm/xóa cư dân thì không cần khởi động lại. API quản trị khuôn mặt trên thiết bị hiện chưa có
xác thực và chạy HTTP không mã hóa; chỉ dùng trong mạng nội bộ được kiểm soát, không giới thiệu như
một kênh có bảo mật.

---

## Slide 11 — Triển khai

**Nội dung trên slide**

- Thành phần cho mỗi cabin:
  - 1 camera IP (RTSP) trong cabin;
  - 1 thiết bị AI Orange Pi A733;
  - kết nối tới IoT Core để đọc trạng thái và gửi lệnh tới bộ điều khiển thang máy.
- Đã tích hợp với bộ điều khiển thang máy INVT qua bảng thanh ghi.
  Các hãng khác: `[CẦN ĐIỀN: danh sách hãng/bộ điều khiển đã hoặc có thể tích hợp]`
- Chạy như dịch vụ hệ thống, tự khởi động lại khi lỗi; camera mất kết nối thì tự kết nối lại.

**Hình ảnh gợi ý:** sơ đồ lắp đặt trong cabin. `[CẦN ĐIỀN: sơ đồ vị trí lắp camera và thiết bị]`

**Ghi chú người thuyết trình:** Thiết bị AI và IoT Core phải đặt trong mạng cô lập (VLAN riêng hoặc
VPN), không mở ra Internet, vì kết nối giữa hai bên chưa mã hóa và chưa xác thực. Không hứa thời điểm bổ
sung mã hóa hay xác thực. Quy trình khảo sát – lắp đặt – đăng ký cư dân – nghiệm thu:
`[CẦN ĐIỀN: quy trình và thời gian triển khai chuẩn]`.

---

## Slide 12 — Phù hợp với

**Nội dung trên slide**

- Tòa nhà có thang máy và danh sách người sử dụng thường xuyên: chung cư, văn phòng, khách sạn,
  nhà máy, trường học, bệnh viện...
- Chủ đầu tư, ban quản lý tòa nhà, đơn vị bảo trì thang máy, nhà thầu cơ điện.

**Hình ảnh gợi ý:** biểu tượng các loại tòa nhà.

**Ghi chú người thuyết trình:** Hỏi khách loại bộ điều khiển thang máy đang dùng để đánh giá khả năng
tích hợp trước khi hứa thời gian triển khai.

---

## Slide 13 — Bước tiếp theo

**Nội dung trên slide**

- Khảo sát hiện trạng thang máy và camera.
- Trình diễn tại chỗ.
- Báo giá: `[CẦN ĐIỀN]`
- Chính sách bảo hành, hỗ trợ: `[CẦN ĐIỀN]`

**Ghi chú người thuyết trình:** Thu thông tin: số thang, số tầng, hãng bộ điều khiển, số người dùng
dự kiến, có camera sẵn hay chưa.

---

## Slide 14 — Liên hệ

**Nội dung trên slide**

- **Công ty DAT Group**
- Website: www.datgroup.com.vn
- Hotline: 18006567
- Email: `[CẦN ĐIỀN]`
- Địa chỉ: `[CẦN ĐIỀN]`

**Hình ảnh gợi ý:** logo, mã QR tới website. `[CẦN ĐIỀN: mã QR]`

---

## Phụ lục — Gợi ý trình diễn tại triển lãm

- Thiết bị có chế độ chạy với webcam USB và màn hình xem trực tiếp, hiển thị khung người, khung mặt,
  tên chào và tầng của người được nhận ra. Có thể đăng ký nhanh khách tham quan bằng vài ảnh chụp tại chỗ.
- Màn hình xem trực tiếp dùng font chữ không hỗ trợ dấu tiếng Việt: nhập tên chào không dấu
  (ví dụ "anh Nam") để hiển thị đúng.
- Nếu không có thang máy thật, phần gọi tầng chỉ trình diễn được trên màn hình.
- Chỉ đăng ký khuôn mặt khách tham quan khi họ đồng ý, và xóa dữ liệu sau sự kiện.
  `[CẦN ĐIỀN: mẫu phiếu đồng ý cho khách tham quan]`
