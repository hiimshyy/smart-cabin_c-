# AIoT Elevator - Smart Cabin — FAQ và xử lý phản đối

> Tài liệu cho nhân viên kinh doanh khi gặp khách hàng và tại triển lãm. Phần A, B, C có thể trích
> gửi khách hàng. Phần D (xử lý phản đối) là tài liệu nội bộ.
> Nguyên tắc: chỉ trả lời theo những gì sản phẩm đang làm được. Câu hỏi có `[CẦN ĐIỀN: ...]` thì
> hẹn khách trả lời sau, không ước lượng.

---

## A. Câu hỏi chung

**1. AIoT Elevator - Smart Cabin là gì?**
Là thiết bị AI đặt trong cabin thang máy. Thiết bị xem video từ camera trong cabin, nhận ra cư dân đã
đăng ký và tự gửi lệnh chọn tầng của cư dân đó tới bộ điều khiển thang máy. Thiết bị cũng ghi dữ liệu
lưu lượng thang máy. Sản phẩm do Công ty DAT Group phát triển.

**2. Có phải thay thang máy hoặc bộ điều khiển không?**
Không thay thang máy. Cần lắp camera IP trong cabin, thiết bị AI và kết nối IoT Core tới bộ điều khiển
thang máy. Hiện đã tích hợp với bộ điều khiển INVT. Với hãng khác cần khảo sát trước:
`[CẦN ĐIỀN: danh sách hãng/bộ điều khiển đã hoặc có thể tích hợp]`.

**3. Nút bấm trong cabin còn dùng được không?**
Có. Hệ thống chỉ thêm tầng được chọn vào bộ điều khiển, không khóa hay thay thế nút bấm.

**4. Đây có phải hệ thống kiểm soát ra vào không?**
Không. Sản phẩm là tiện ích chọn tầng tự động. Người không được nhận ra vẫn dùng thang bình thường
bằng nút bấm.

**5. Khách hoặc người lạ đi thang thì sao?**
Hệ thống không gọi tầng cho người không nhận ra. Họ bấm nút như bình thường.

**6. Nhiều người vào cùng lúc thì sao?**
Mỗi cư dân được nhận ra sẽ có tầng của mình được gọi một lần trong lượt đi thang đó. Nhiều người
cùng tầng chỉ tạo một lệnh. Các tầng khác nhau được gửi lần lượt.

**7. Cư dân ở tầng 7 nhưng hay xuống hầm gửi xe thì sao?**
Mỗi cư dân có tầng chính và có thể khai báo thêm tầng phụ (ví dụ tầng hầm B). Hệ thống chọn tầng theo
vị trí và chiều chạy hiện tại của thang: vào thang ở tầng 7 khi thang đi xuống thì gọi B, vào ở B thì
gọi tầng 7.

**7a. Vì sao có lúc cư dân được nhận ra nhưng thang không tự chọn tầng?**
Hệ thống không gọi tầng khi thang đang chạy ngược chiều với tầng cần đến. Ví dụ: cư dân chỉ khai báo
tầng 7, vào thang ở tầng 3 khi thang đang đi xuống. Trong lượt đi thang đó hệ thống không xét lại cư dân
này; cư dân bấm nút như bình thường. Lượt đi thang kết thúc khi cabin trống 5 giây. Hệ thống cũng không
gọi tầng khi cabin đang ở đúng tầng của cư dân hoặc tầng nằm ngoài dải tầng đã cấu hình.

**8. Nếu hệ thống gọi sai tầng?**
Lệnh của hệ thống chỉ thêm một tầng; cư dân vẫn bấm được tầng khác. Hệ thống không tự hủy tầng đã
chọn. Hủy tầng đã chọn theo chức năng có sẵn của thang máy: `[CẦN ĐIỀN: cách hủy tầng trên thang của khách]`.

**9. Mất bao lâu từ khi bước vào đến khi tầng được chọn?**
Hệ thống xác nhận danh tính khi khuôn mặt khớp ổn định qua 5 khung hình liên tiếp (mặc định).
Thời gian thực tế tại công trình: `[CẦN ĐIỀN: thời gian đo thực tế]`.

**10. Độ chính xác nhận diện là bao nhiêu?**
`[CẦN ĐIỀN: tỉ lệ nhận nhầm / bỏ sót đo trên dữ liệu thật]`. Ngưỡng nhận diện điều chỉnh được theo
từng cabin để cân bằng giữa nhận nhầm và bỏ sót.

**11. Hệ thống nhận diện được tối đa bao nhiêu cư dân?**
`[CẦN ĐIỀN: số cư dân tối đa đã kiểm chứng]`. Mỗi mẫu khuôn mặt chiếm 2 KB dữ liệu.

**12. Đeo khẩu trang, kính, đội mũ, ánh sáng yếu có nhận được không?**
`[CẦN ĐIỀN: kết quả thử nghiệm]`. Hệ thống cần nhìn thấy khuôn mặt. Đăng ký nhiều ảnh ở các góc và
điều kiện ánh sáng khác nhau giúp nhận diện tốt hơn, vì hệ thống so khớp với mẫu giống nhất của mỗi người.

**13. Ngoại hình thay đổi (cắt tóc, để râu, trẻ em lớn lên) thì sao?**
Quản trị viên bổ sung ảnh mới cho cư dân. Ảnh mới có hiệu lực ngay, không cần khởi động lại thiết bị.

**14. Đăng ký cư dân như thế nào?**
Quản trị viên tải ảnh khuôn mặt của cư dân lên qua giao diện quản trị web, kèm mã cư dân, họ tên,
tầng chính, tầng phụ. Cư dân mới được nhận diện ngay sau khi đăng ký.
`[CẦN ĐIỀN: quy trình đăng ký chuẩn cho ban quản lý và yêu cầu về ảnh]`

**15. Có tầng hầm được không?**
Có. Hệ thống hỗ trợ tầng hầm B.

**16. Một tòa nhà nhiều thang thì sao?**
Mỗi cabin cần camera riêng; mỗi tiến trình phần mềm phục vụ một cabin.
`[CẦN ĐIỀN: cấu hình phần cứng cho tòa nhà nhiều thang]`

**17. Dữ liệu lưu lượng thang máy dùng để làm gì?**
Ban quản lý biết được tầng đi – tầng đến phổ biến theo điểm dừng, giờ cao điểm theo từng chiều, thời
gian chuyến và thời gian chờ tại sảnh. Dữ liệu gắn cờ khi thang ở chế độ bảo trì, lỗi, khóa, báo cháy
hoặc quá tải. Dữ liệu chuyến đi của từng hành khách và số người trong cabin đang hoàn thiện, chưa kiểm
chứng thực địa. Phiên bản hiện tại xuất file CSV, chưa có giao diện báo cáo web.

**18. Có kết nối với phần mềm quản lý tòa nhà (BMS) hoặc ứng dụng cư dân không?**
Phiên bản hiện tại chưa có giao diện tích hợp dành cho hệ thống bên ngoài. Dữ liệu lưu lượng xuất được
dạng file CSV. Thiết bị có API REST nội bộ phục vụ giao diện quản trị đăng ký khuôn mặt; API này chưa có
xác thực và mã hóa nên không dùng làm điểm tích hợp với hệ thống bên ngoài. Tích hợp khác: `[CẦN ĐIỀN]`.

**19. Có phát hiện vật nuôi trong thang không?**
Có. Hệ thống phát hiện chó và mèo trong cabin, ghi cảnh báo và lưu ảnh chụp sự kiện trên thiết bị.
Ảnh chụp là nguyên khung hình camera nên có thể có hình người đang ở trong cabin (xem câu 30 và 36).
Phiên bản hiện tại chưa gửi cảnh báo này ra ứng dụng hay hệ thống khác.

**20. Có đếm được số người trong cabin không?**
Tính năng đang hoàn thiện, chưa kiểm chứng thực địa. Phần mềm đếm lượt đi qua vùng cửa; khi điều kiện
không cho phép đếm chắc chắn (mất camera, mất dấu người ở vùng cửa), hệ thống đánh dấu số đếm là
"không chắc chắn". Camera cần nhìn thấy toàn bộ cửa; nhiều người chồng lấn có thể bị đếm thiếu.
Độ chính xác: `[CẦN ĐIỀN]`.

---

## B. Câu hỏi kỹ thuật và vận hành

**21. Phần cứng gồm những gì?**
Bo mạch Orange Pi A733 có NPU tích hợp, chạy 3 mô hình AI: YOLOv5s (phát hiện người, chó, mèo),
SCRFD 2.5G (phát hiện khuôn mặt), MobileFaceNet (trích đặc trưng khuôn mặt 512 chiều). Thêm một
camera IP (RTSP) trong cabin. Chi tiết xem Datasheet.

**22. Tốc độ xử lý?**
Tốc độ ở chế độ vận hành (phát hiện người + phát hiện và nhận diện khuôn mặt + theo dõi):
`[CẦN ĐIỀN: FPS chế độ vận hành]`. Trên camera IP tại công trình: `[CẦN ĐIỀN]`. Thời gian chạy trên
NPU đo nội bộ (chế độ chỉ phát hiện + nhận diện khuôn mặt, webcam USB): phát hiện khuôn mặt khoảng
10–17 ms, nhận diện khoảng 3 ms mỗi khuôn mặt.

**23. Mất Internet thì sao?**
Xử lý AI và so khớp chạy trên thiết bị, không cần cloud. Thiết bị cần kết nối mạng nội bộ tới camera
và IoT Core. `[CẦN ĐIỀN: yêu cầu Internet cho quản trị từ xa, nếu có]`

**24. Mất kết nối camera hoặc IoT Core thì sao?**
Camera mất tín hiệu: thiết bị tự kết nối lại với thời gian chờ tăng dần (0,5 s đến 10 s). IoT Core mất
kết nối: lệnh gọi tầng chưa gửi được quá 5 s sẽ bị hủy, không gửi lại lệnh cũ khi kết nối phục hồi.
Thang máy vẫn hoạt động bình thường bằng nút bấm.

**25. Thiết bị bị treo hoặc mất điện?**
Phần mềm chạy như dịch vụ hệ thống, tự khởi động cùng thiết bị và tự khởi động lại sau 5 s nếu dừng.
Số đếm người được lưu xuống đĩa và khôi phục sau khi khởi động lại.

**26. Thiết bị có chạy AI liên tục không?**
Không. Khi có tín hiệu cửa từ bộ điều khiển, thiết bị bắt đầu phát hiện người lúc cửa mở và ngừng chạy
các mô hình khi cửa đóng và cabin trống. Nếu mất tín hiệu cửa, thiết bị vẫn quét người ở tần suất thấp.

**27. Có ghi video không?**
Có tùy chọn ghi video khi có người trong cabin (mặc định tắt), lưu trên thiết bị. Video này ghi hình
tất cả người trong cabin, nên chỉ bật khi đơn vị vận hành đã có căn cứ pháp lý. Phiên bản hiện tại
chưa có xóa video tự động (xem câu 36).

**28. Theo dõi tình trạng thiết bị thế nào?**
Thiết bị ghi định kỳ mỗi 30 s: CPU, RAM, dung lượng ổ đĩa, nhiệt độ CPU/NPU. Nhật ký hệ thống lưu
theo ngày, giữ 14 ngày (mặc định). `[CẦN ĐIỀN: kênh giám sát từ xa và quy trình hỗ trợ]`

**29. Bảo hành, bảo trì, cập nhật phần mềm?**
`[CẦN ĐIỀN]`

---

## C. Dữ liệu cá nhân và quyền riêng tư

> Phần này cung cấp thông tin về cách sản phẩm xử lý dữ liệu, **không phải tư vấn pháp lý**. Khách hàng
> cần tham khảo luật sư của mình.

### Khung pháp lý tham chiếu

- **Luật Bảo vệ dữ liệu cá nhân số 91/2025/QH15**, hiệu lực từ 01/01/2026.
- **Nghị định 356/2025/NĐ-CP** hướng dẫn Luật, hiệu lực từ 01/01/2026, **thay thế Nghị định
  13/2023/NĐ-CP** ([LuatVietnam](https://luatvietnam.vn/legal-updates/new-features-of-decree-356-2025-compared-with-decree-13-2023-on-personal-data-protection-892-106712-article.html)).
  Khi khách hàng nhắc tới Nghị định 13/2023, cần lưu ý văn bản này đã được thay thế.

`[CẦN ĐIỀN: bộ phận pháp chế DAT Group rà soát và xác nhận phần này trước khi phát hành]`

**30. Hệ thống có lưu ảnh khuôn mặt không?**
Có, với cư dân đã đăng ký: ảnh đăng ký được lưu trên thiết bị, cùng với mẫu đặc trưng khuôn mặt dạng
vector số dùng để so khớp.

Với mọi người trong cabin, kể cả người chưa đăng ký:

- **Ảnh cảnh báo chó/mèo**: mỗi lần phát hiện chó hoặc mèo, thiết bị lưu nguyên khung hình camera, nên
  ảnh có thể có hình bất kỳ ai đang ở trong cabin. Ảnh lưu trên thiết bị, phần mềm không gửi ảnh này qua
  mạng. Ảnh dùng cho gỡ lỗi nội bộ và nghiên cứu phát triển (R&D) của DAT Group, không cung cấp ra bên
  ngoài. Phiên bản hiện tại chưa có xóa tự động (xem câu 36).
- **Video sự kiện** (nếu bật, mặc định tắt): ghi hình tất cả người trong cabin (xem câu 27).

Riêng mẫu và ảnh khuôn mặt của người chưa đăng ký, tùy cấu hình của từng công trình:

- Khi tắt nhận diện lại khách vãng lai và tắt lưu ảnh theo chuyến: không lưu mẫu khuôn mặt, không lưu
  ảnh khuôn mặt theo chuyến; hệ thống chỉ ghi một sự kiện "không nhận ra" không gắn danh tính.
- Khi bật: thiết bị lưu mẫu đặc trưng khuôn mặt của khách (giữ 90 ngày từ lần thấy cuối) và có thể lưu
  ảnh khuôn mặt 112×112 theo chuyến (giữ 30 ngày), gắn mã số ẩn danh, không có tên.

Cấu hình tại công trình của quý khách: `[CẦN ĐIỀN: bật/tắt nhận diện lại khách vãng lai và lưu ảnh khách]`.

**31. Dữ liệu có gửi lên cloud không?**
Không gửi video. Nhận diện, so khớp, dữ liệu cư dân và dữ liệu lưu lượng nằm trên thiết bị tại tòa nhà.
Phiên bản hiện tại không gửi dữ liệu lưu lượng ra mạng. Hồ sơ cư dân trên nền tảng quản trị:
`[CẦN ĐIỀN: vị trí lưu trữ cơ sở dữ liệu quản trị]`.

**32. Dữ liệu khuôn mặt thuộc loại nào theo luật?**
Theo tài liệu dự án, ảnh và đặc trưng khuôn mặt là dữ liệu sinh trắc học, thuộc nhóm dữ liệu cá nhân
nhạy cảm theo Luật 91/2025/QH15. Phải có căn cứ xử lý hợp pháp (ví dụ sự đồng ý của cư dân) trước khi
đăng ký khuôn mặt.

**33. Ai chịu trách nhiệm tuân thủ?**
Đơn vị vận hành tòa nhà quyết định mục đích và phạm vi thu thập dữ liệu. Vai trò pháp lý của DAT Group
trong từng hợp đồng: `[CẦN ĐIỀN]`. Trước khi vận hành, đơn vị vận hành cần:

1. Niêm yết thông báo tại thang máy: mục đích thu thập, phạm vi xử lý, đầu mối liên hệ.
2. Có căn cứ xử lý cho từng cư dân đăng ký (ví dụ văn bản đồng ý). `[CẦN ĐIỀN: mẫu văn bản đồng ý]`
   Văn bản đồng ý về sử dụng hình ảnh mà cư dân ký khi vào ở cần nêu đủ các mục đích: chọn tầng tự
   động, thống kê lưu lượng, ảnh cảnh báo chó/mèo dùng cho gỡ lỗi nội bộ và R&D của DAT Group, video sự
   kiện (nếu bật). Thông báo niêm yết ở mục 1 cũng cần nêu các mục đích này cho khách đến tòa nhà.
   Soạn và thu văn bản đồng ý là trách nhiệm của đơn vị vận hành tòa nhà.
3. Xác định thời hạn lưu và quy trình xóa khi có yêu cầu.
4. Kiểm soát người được truy cập thiết bị và dữ liệu.
5. Thực hiện các nghĩa vụ khác theo Luật và Nghị định hướng dẫn (ví dụ hồ sơ đánh giá tác động xử lý
   dữ liệu cá nhân) theo tư vấn pháp lý.

**34. Người không đăng ký có bị nhận diện không?**
Camera nhìn thấy mọi người trong cabin, và thiết bị so khuôn mặt của mọi người với danh sách cư dân.
Tính năng nhận diện lại khách vãng lai qua nhiều chuyến (để thống kê lưu lượng) là tùy chọn, bật hay
tắt theo từng công trình, chỉ nên bật sau khi hoàn tất thủ tục pháp lý. Khi tắt, người không khớp không
bị lưu mẫu khuôn mặt hay ảnh khuôn mặt theo chuyến. Khi bật, thiết bị lưu mẫu khuôn mặt của khách vãng
lai kèm một mã số ẩn danh, không có tên. Dù bật hay tắt, người không đăng ký vẫn có thể có hình trong
ảnh cảnh báo chó/mèo và trong video sự kiện nếu bật (xem câu 30).

**35. Cư dân yêu cầu xóa dữ liệu thì làm thế nào?**
Quản trị viên xóa cư dân trên giao diện quản trị. Hệ thống xóa hồ sơ, thư mục ảnh và toàn bộ mẫu khuôn
mặt của cư dân đó trên thiết bị. Dữ liệu khách vãng lai được xóa bằng công cụ quản trị trên thiết bị.

Thao tác xóa cư dân hiện **chưa** xóa các dữ liệu sau:

- Lịch sử chuyến đi gắn mã cư dân (tầng vào, tầng ra, thời gian) trong dữ liệu lưu lượng: giữ đến khi
  hết thời hạn lưu lượng (mặc định 730 ngày).
- Nhật ký nhận diện (thời gian, cabin, điểm tương đồng): các dòng vẫn còn nhưng không còn liên kết với
  cư dân đã xóa.
- Ảnh khuôn mặt theo chuyến của cư dân, khi chế độ lưu ảnh theo chuyến là "tất cả": giữ đến khi hết
  thời hạn (mặc định 30 ngày, tối đa 5 GB).
- Ảnh cảnh báo chó/mèo và video sự kiện (nếu bật) có hình cư dân: chưa có xóa tự động.

`[CẦN ĐIỀN: quy trình xóa bổ sung cho lịch sử chuyến đi, nhật ký nhận diện và ảnh/video khi cư dân yêu cầu xóa]`

**36. Dữ liệu được giữ bao lâu?**

| Dữ liệu | Thời hạn mặc định (cấu hình được) |
|---|---|
| Dữ liệu lưu lượng (chuyến, điểm dừng, gọi tầng, chuyến gắn mã cư dân) | 730 ngày |
| Mẫu khuôn mặt khách vãng lai (nếu bật) | 90 ngày từ lần thấy cuối; sau đó chỉ còn mã số ẩn danh |
| Ảnh khuôn mặt theo chuyến (nếu bật) | 30 ngày, tối đa 5 GB |
| Nhật ký hệ thống | 14 ngày |
| Dữ liệu cư dân đã đăng ký | Đến khi bị xóa thủ công |
| Nhật ký nhận diện cư dân | Phiên bản hiện tại chưa có xóa tự động; giữ đến khi xóa thủ công. `[CẦN ĐIỀN: chính sách và cách xóa định kỳ]` |
| Ảnh cảnh báo chó/mèo (nguyên khung hình) | Phiên bản hiện tại chưa có xóa tự động. `[CẦN ĐIỀN: thời hạn lưu ảnh cảnh báo chó/mèo]` |
| Video sự kiện (nếu bật) | Phiên bản hiện tại chưa có xóa tự động. `[CẦN ĐIỀN: thời hạn lưu video sự kiện]` |

Dữ liệu lưu lượng, mẫu khách vãng lai và ảnh theo chuyến hết hạn được tự động xóa mỗi giờ. Nhật ký hệ
thống được giữ 14 ngày. Ảnh cảnh báo chó/mèo và video sự kiện chưa có xóa tự động.

**37. Dữ liệu được bảo vệ như thế nào?**
- Dữ liệu nằm trên thiết bị tại tòa nhà.
- Nhật ký hệ thống chỉ ghi mã số, không ghi tên, tên chào, căn hộ, vector khuôn mặt hay đường dẫn ảnh.
  Thông tin đăng nhập camera được ẩn trong nhật ký.
- Cơ sở dữ liệu lưu lượng chỉ tài khoản dịch vụ đọc được.
- API quản trị khuôn mặt trên thiết bị hiện chưa có xác thực và chưa mã hóa kết nối. Thiết bị phải đặt
  trong mạng nội bộ được kiểm soát, chỉ người được phép mới truy cập được mạng đó.
- Kết nối giữa thiết bị và IoT Core (gửi lệnh gọi tầng, đọc trạng thái cửa và tầng) hiện chưa mã hóa
  (ws://) và chưa xác thực (thiết bị chỉ gửi mã thiết bị). Thiết bị và IoT Core phải nằm trong mạng cô
  lập (VLAN riêng hoặc VPN), không mở ra Internet.
- Ảnh cảnh báo chó/mèo lưu trên thiết bị với quyền file chỉ tài khoản sở hữu đọc được (0600).
- Mã hóa dữ liệu lưu trữ: `[CẦN ĐIỀN]`.

---

## D. Xử lý phản đối (nội bộ)

| Khách nói | Cách trả lời | Căn cứ |
|---|---|---|
| "Lo ngại quyền riêng tư, cư dân không thích bị camera nhận diện." | Xử lý tại chỗ, không gửi video lên cloud. Chỉ cư dân đồng ý mới được đăng ký. Nhận diện lại và lưu ảnh khách vãng lai là tùy chọn, công trình có thể tắt; khi tắt, người không đăng ký không bị lưu mẫu khuôn mặt hay ảnh khuôn mặt theo chuyến. Nói rõ: ảnh cảnh báo chó/mèo là nguyên khung hình nên có thể có hình người trong cabin; ảnh nằm trên thiết bị, chỉ dùng cho gỡ lỗi nội bộ và R&D của DAT Group, không cung cấp ra ngoài. Văn bản đồng ý của cư dân do tòa nhà soạn cần nêu mục đích này. Nhật ký hệ thống không ghi tên. Cư dân không muốn tham gia vẫn đi thang bằng nút bấm. Lưu ý nội bộ: cấu hình triển khai mẫu hiện tại đang **bật** cả hai tính năng này; chỉ hứa "tắt" khi cấu hình bàn giao đã được xác nhận tắt. | Mục C.30, C.31, C.33, C.34; `[CẦN ĐIỀN: cấu hình bàn giao cho khách hàng]` |
| "Nhận nhầm người thì gọi sai tầng." | Hệ thống chỉ xác nhận khi khớp ổn định qua nhiều khung hình, ngưỡng điều chỉnh theo cabin. Nếu nhận nhầm, thang dừng thêm ở một tầng không cần thiết; nút bấm vẫn dùng được. Số liệu độ chính xác: hẹn gửi sau. | Mục A.8, A.10; `[CẦN ĐIỀN: số liệu độ chính xác]` |
| "Cầm ảnh in là lừa được." | Trả lời thẳng: phiên bản hiện tại chưa có chống giả mạo bằng ảnh/video. Vì sản phẩm chỉ chọn tầng, không mở cửa hay cấp quyền ra vào, nên giả mạo chỉ làm thang dừng thêm ở một tầng. Không hứa tính năng chống giả mạo khi chưa có lộ trình. | Datasheet mục 13; `[CẦN ĐIỀN: lộ trình chống giả mạo, nếu có]` |
| "Thang máy của tôi hãng khác." | Hiện đã tích hợp bộ điều khiển INVT qua IoT Core. Với hãng khác, đề nghị khảo sát kỹ thuật trước khi cam kết. | `[CẦN ĐIỀN: danh sách hãng tương thích]` |
| "Tòa nhà tôi đã có thẻ từ." | Sản phẩm không thay thẻ từ; sản phẩm bổ sung chọn tầng tự động và dữ liệu lưu lượng thang máy. Tích hợp với hệ thống thẻ: chưa có. | Mục A.4 |
| "Mất mạng, hỏng thiết bị thì thang có bị ảnh hưởng không?" | Thang hoạt động bình thường bằng nút bấm. Thiết bị tự khởi động lại, camera tự kết nối lại, lệnh cũ không bị gửi bù. | Mục B.24, B.25 |
| "Thêm cư dân mới có phức tạp không?" | Đăng ký qua giao diện web bằng ảnh, có hiệu lực ngay, không cần khởi động lại. | Mục A.14 |
| "Giá bao nhiêu?" | Không báo giá miệng. Ghi nhận số thang, số tầng, hãng bộ điều khiển, số cư dân, hiện trạng camera để lập báo giá. | `[CẦN ĐIỀN: chính sách báo giá]` |
| "Đã có ai dùng chưa?" | Không nêu tên khách hàng hay kết quả thí điểm khi chưa được phép công bố. Đề nghị trình diễn hoặc khảo sát tại chỗ. | `[CẦN ĐIỀN: khách hàng tham chiếu được phép công bố]` |
| "Dữ liệu lưu lượng thì xem ở đâu?" | Phiên bản hiện tại xuất file CSV theo khoảng ngày. Chưa có giao diện báo cáo web. | Mục A.17 |
| "Cabin của tôi có camera sẵn rồi." | Thiết bị dùng camera IP qua RTSP; cần kiểm tra camera hiện có về vị trí, góc nhìn và chuẩn nén. | `[CẦN ĐIỀN: yêu cầu camera tối thiểu]` |

### Những điều không được nói

- Không đưa con số độ chính xác, tốc độ thực tế, số cư dân tối đa khi các mục `[CẦN ĐIỀN]` tương ứng
  chưa được điền.
- Không nói sản phẩm có chống giả mạo, có chào bằng giọng nói, có giao diện báo cáo web, có đẩy cảnh báo
  ra ứng dụng hay hỗ trợ nhiều camera mỗi cabin: phiên bản hiện tại chưa có các tính năng này.
- Không nói giao diện/API quản trị trên thiết bị có xác thực hoặc mã hóa kết nối: phiên bản hiện tại
  chưa có cả hai.
- Không nói kết nối giữa thiết bị và IoT Core có mã hóa hoặc xác thực: phiên bản hiện tại chưa có cả
  hai. Không hứa thời điểm bổ sung các tính năng này.
- Không nói nhận diện lại khách vãng lai "mặc định tắt" khi chưa xác nhận cấu hình bàn giao.
- Không nói người không đăng ký không bao giờ bị chụp ảnh hay ghi hình: ảnh cảnh báo chó/mèo là nguyên
  khung hình camera, và video sự kiện (nếu bật) ghi hình mọi người trong cabin.
- Không nói mọi dữ liệu đều tự động xóa khi hết hạn: ảnh cảnh báo chó/mèo, video sự kiện, nhật ký nhận
  diện và dữ liệu cư dân chưa có xóa tự động.
- Không nói xóa cư dân là xóa toàn bộ dữ liệu liên quan: lịch sử chuyến đi, nhật ký nhận diện, ảnh theo
  chuyến (chế độ "tất cả") và ảnh/video có hình cư dân vẫn còn (xem câu 35).
- Không trình bày đếm người và dữ liệu từng hành khách như tính năng đã hoàn thiện.
- Không cam kết "tuân thủ pháp luật" thay khách hàng; chỉ nói sản phẩm hỗ trợ đơn vị vận hành thực hiện
  nghĩa vụ.
- Không nêu tên khách hàng, kết quả thí điểm, giá, hoặc so sánh với đối thủ khi chưa có tài liệu được duyệt.
