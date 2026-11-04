# Requirements Document

## Introduction

Spec này mô tả tính năng **Nhật ký di chuyển thang máy** (tập dữ liệu Traffic). Mục tiêu là xây dựng tập dữ liệu traffic cho thang máy: ma trận tầng đi – tầng đến, lưu lượng theo giờ và theo chiều, thời gian chuyến, tải cabin và thời gian chờ tại sảnh.

- Phạm vi v1: một thang (thang 2, `cabin_id=2`, slave `-2`), một camera (`cam2`, `config/occupancy_cam2.json`). Thang 2 phục vụ tầng B đến tầng 9.
- Nguồn dữ liệu:
  - Sự kiện Entry/Exit của bộ đếm người (spec `cabin-occupancy`).
  - Danh tính theo track của nhận diện khuôn mặt (spec `resident-db-layer`).
  - Thanh ghi bộ điều khiển INVT nhận qua Socket.IO `CONN_DATA`, theo bảng `docs/Address_Elevator.docx`.
- Dữ liệu lưu cục bộ trên thiết bị.

## Glossary

- **Điểm dừng (stop)**: khoảng thời gian cabin đứng yên tại một tầng, từ lúc dừng tới lúc chạy lại.
- **Chuyến cabin (cabin run)**: cabin rời một điểm dừng và tới điểm dừng kế tiếp.
- **Chuyến hành khách (passenger trip)**: một người từ lúc vào cabin (Entry) tới lúc ra (Exit).
- **Occupant lineage**: chuỗi track ID của cùng một người trong cabin, kể cả khi track đổi ID và được bộ đếm chuyển tiếp qua ghost.
- **Visitor**: hồ sơ ẩn danh của người chưa đăng ký. Hồ sơ gồm `visitor_id` dạng số và các embedding khuôn mặt.
- **Gọi tầng ngoài (hall call)**: lệnh gọi bằng nút ở sảnh tầng, theo chiều lên hoặc xuống.
- **Chỉ số tầng (floor index)**: số nguyên dùng thống nhất trong mọi bản ghi, theo đúng quy ước của lệnh gọi tầng hiện có (giá trị `2^n` gửi tới `8193-2` nghĩa là tầng `n`). Tầng hiển thị là `B` khi chỉ số bằng 0, các tầng còn lại hiển thị đúng bằng chỉ số. Quy đổi đã chốt từ dữ liệu thật: chỉ số tầng = `8465 − 1` = bit `n` của `8211`/`8215`/`8225` (xem phần "Kết quả ghi mẫu `CONN_DATA`").

## Requirements

### R1. Đọc trạng thái thang từ thanh ghi

1. Mở rộng subscription `CONN_DATA` hiện có để đọc thêm các thanh ghi sau. Địa chỉ dạng `<địa chỉ cloud>-<slave>`, slave mặc định là `2`.

   | Thanh ghi | Ý nghĩa | Dùng cho |
   |---|---|---|
   | `8465` | Tầng hiện tại (cơ số 10, 1 = B) | Tầng vào/ra, tầng đi/đến |
   | `8464` | Chiều cao cabin (/100 m) | Lưu kèm điểm dừng để kiểm tra chéo tầng |
   | `8480` | Trạng thái: bit0 lên, bit1 xuống, bit2 đang chạy, bit3 bảo trì, bit5 lỗi, bit6 khóa, bit7 báo cháy, bit11 độc lập | Phát hiện điểm dừng/chuyến chạy, cờ chế độ đặc biệt |
   | `8472` | Đầu ra cửa: bit5 mở, bit6 đóng (đã dùng) | Giờ mở/đóng cửa |
   | `8471` | Đầu vào CTB: bit2 quá tải, bit3 cửa mở hết, bit4 cửa đóng hết | Giờ cửa mở/đóng thực tế, cờ quá tải |
   | `8467` | Bit14 bằng tầng | Xác nhận điểm dừng |
   | `8211` | Gọi tầng trong cabin, mỗi bit một tầng | Tầng hành khách bấm |
   | `8215` | Gọi tầng ngoài chiều lên, mỗi bit một tầng | Thời gian chờ, lưu lượng theo chiều |
   | `8225` | Gọi tầng ngoài chiều xuống, mỗi bit một tầng | Thời gian chờ, lưu lượng theo chiều |
   | `8448`/`8449` | Tổng số lần chạy | Đối chiếu số chuyến cabin |

2. `CONN_DATA` chỉ gửi các thanh ghi đã đổi giá trị, trừ message đầu tiên sau khi subscribe là snapshot đầy đủ. Khi thang đứng yên có thể không có message nào trong hơn 2 phút. Vì vậy:
   - Hệ thống giữ giá trị cuối của từng thanh ghi. Giá trị cuối vẫn hợp lệ khi chưa có message mới, miễn là socket vẫn kết nối và cùng session với snapshot đã nhận.
   - Không dùng quy tắc "phải có dữ liệu trong 5 s" cho dữ liệu traffic.
   - Coi là mất dữ liệu (`register_gap`) khi: socket mất kết nối hoặc đổi session; inbox tràn; hoặc cabin đang chạy mà không có message nào của slave đó trong `--traffic-run-timeout-ms` (mặc định 60000; một chuyến B–9 thực tế dưới 30 s).
   - Sau khi kết nối lại, snapshot đầy đủ đầu tiên khôi phục trạng thái.
3. Mỗi message được đóng dấu bằng steady clock ngay trên socket worker. Frame thread đọc dữ liệu mà không bị chặn. Trường `TS` của gateway được lưu kèm để đối chiếu.
4. Độ phân giải thời gian của mọi mốc lấy từ thanh ghi bằng chu kỳ gửi của gateway (đo được khoảng 5,6 s cho mỗi slave). Bản ghi lưu thời điểm nhận message, không nội suy.
5. Giá trị ngoài miền bị bỏ qua (ví dụ tầng ngoài dải B–9 sau khi quy đổi, số không phải 16 bit). Cảnh báo được log có giới hạn tần suất. Dải tầng hợp lệ cấu hình được (`--traffic-floor-min`, `--traffic-floor-max`, mặc định 0 và 9).
6. Không thay đổi định dạng và hành vi của lệnh gọi tầng hiện có (`8193-2`).
7. Gọi tầng ngoài dùng chung cho cả nhóm thang: bit của `8215`/`8225` trên slave `-1` và `-2` giống nhau, lệch nhau 1–2 s. Hệ thống theo dõi điểm dừng của thang 1 (slave `-1`, có sẵn trong cùng luồng dữ liệu) để biết lệnh gọi ngoài nào do thang 1 phục vụ.

### R2. Chuyến cabin và điểm dừng

1. Điểm dừng và chuyến chạy được suy ra từ bit đang chạy của `8480`, hướng chạy, tầng `8465` và tín hiệu cửa.
2. Mỗi chuyến cabin ghi: tầng đi, tầng đến, giờ rời, giờ tới, hướng, số người lúc rời (count và confidence của occupancy), danh sách chuyến hành khách đang ở trong cabin, và mask các tầng đang được gọi trong cabin lúc rời. Nếu count lớn hơn sức chứa cabin (`--traffic-cabin-capacity`, mặc định 15), count được lưu NULL và gắn cờ `load_implausible`. Bộ đếm production hiện đang ở mức 275, nên cờ này sẽ bật cho tới khi occupancy được reconcile.
3. Mỗi điểm dừng ghi: tầng, giờ tới, giờ cửa mở, giờ cửa đóng, giờ rời, số người lên và số người xuống.
4. Khi thang ở chế độ bảo trì, lỗi, khóa, báo cháy, độc lập hoặc quá tải, bản ghi vẫn được lưu và gắn cờ tương ứng.
5. Nếu mất dữ liệu thanh ghi giữa chừng, bản ghi được đóng với cờ `register_gap`. Hệ thống không suy đoán tầng.
6. Số chuyến cabin phát hiện được phải khớp với chênh lệch bộ đếm `8448`/`8449`. Chênh lệch được log ở mức warning.

### R3. Chuyến hành khách

1. Mỗi Entry mở một chuyến gắn với một occupant lineage. Exit của cùng lineage đóng chuyến đó.
2. Tầng vào là tầng của điểm dừng chứa thời điểm Entry. Tầng ra là tầng thực tế của điểm dừng chứa thời điểm Exit. Dung sai giữa video và thanh ghi bằng một chu kỳ gửi của gateway (`--traffic-floor-tolerance-ms`, mặc định 6000), vì mốc tới/rời điểm dừng có thể được nhận trễ tới một chu kỳ.
3. Mỗi chuyến ghi: khóa chuyến, giờ vào, giờ ra, tầng vào, tầng ra, loại danh tính (`resident`/`visitor`/`unidentified`), `resident_id`, `visitor_id`, độ tương đồng, tầng đã gọi tự động và kết quả gọi, mask các tầng được bấm trong lúc người đó ở cabin, số chuyến cabin đã đi, gọi tầng ngoài đã được phục vụ ở điểm dừng lúc vào (nếu có, xem R13), mức tin cậy và cờ lý do.
4. Exit không có Entry tương ứng (ví dụ count khôi phục sau restart) tạo chuyến chỉ có đầu ra: tầng vào để trống, mức tin cậy thấp, cờ `entry_unobserved`.
5. Entry không có Exit được đóng khi occupancy được reconcile về 0, khi app tắt, hoặc sau `max_trip_ms` (mặc định 15 phút). Tầng ra để trống và gắn cờ `exit_unobserved`.
6. Vào rồi ra ở cùng tầng mà cabin không chạy vẫn được ghi, với cờ `no_ride`.
7. Nếu occupancy đang `Uncertain` lúc Entry hoặc Exit, chuyến vẫn được ghi với mức tin cậy thấp.

### R4. Người đã đăng ký

1. Danh tính resident lấy từ outcome `confirmed` của `InteractionManager` trên bất kỳ track nào thuộc lineage, kể cả track quan sát được trong tối đa 10 s trước Entry.
2. Nếu một lineage có nhiều resident khác nhau, hệ thống ghi resident có độ tương đồng cao nhất và gắn cờ `identity_conflict`.
3. Người đã đăng ký nhưng không được nhận ra sẽ được xử lý như người chưa đăng ký.
4. Ghi tầng gọi tự động, trạng thái cuối của lệnh (`AckedSuccess`, `AckedError`, `Expired`, ...), và kết quả xác nhận qua thanh ghi: bit tương ứng của `8211` có bật trong vòng `--traffic-call-confirm-ms` (mặc định 15000) sau khi gửi lệnh hay không. Lý do: trong log production, 15/16 lệnh gọi ngày 02/11 và 18/18 lệnh gọi ngày 03/11 bị đánh dấu `ACKED_ERROR reason=MALFORMED_ACK`, nên ACK hiện không cho biết lệnh có được đăng ký thật không.

### R5. Người chưa đăng ký: nhận lại qua nhiều chuyến

1. Trong lúc chuyến đang mở, hệ thống giữ tối đa K embedding (mặc định 5) có chất lượng tốt nhất từ các khuôn mặt thuộc lineage mà không khớp resident nào.
2. Khi chuyến đóng, các embedding được so với thư viện visitor:
   - Khớp đủ ngưỡng `visitor_match_thr` và đủ khoảng cách với ứng viên thứ hai: gán `visitor_id` cũ.
   - Không khớp nhưng có ít nhất 2 mẫu nhất quán: tạo visitor mới.
   - Không đủ mẫu: ghi `unidentified`.
3. Mỗi visitor giữ tối đa M embedding (mặc định 10). Khi đầy, mẫu chất lượng thấp nhất bị thay.
4. Nếu resident gần khớp nhất có độ tương đồng nằm trong dải sát dưới ngưỡng resident, chuyến ghi thêm `suggested_resident_id` và độ tương đồng để xác minh sau.
5. Ngưỡng visitor cao hơn ngưỡng resident để hạn chế gộp nhầm hai người khác nhau.
6. Việc so khớp visitor không chạy trên frame thread.
7. Tính năng nhận lại visitor bật/tắt bằng cấu hình (`--traffic-visitor-reid`). Khi tắt, mọi người chưa đăng ký đều ghi `unidentified` và không lưu embedding.

### R6. Xác minh danh tính sau

1. Thêm công cụ CLI `traffic_admin` với các lệnh: liệt kê/xem visitor, gộp hai visitor, xóa visitor, nâng visitor thành resident (resident mới hoặc resident có sẵn), liệt kê đề xuất ghép, và export.
2. Khi nâng visitor thành resident:
   - Chép embedding của visitor vào bảng `embeddings` của `residents.db` với `source='cabin'`.
   - Cập nhật mọi chuyến của visitor sang `resident_id`.
   - Ghi audit.
   - App nạp embedding mới qua cơ chế hot reload hiện có.
3. Đề xuất ghép: so embedding của resident với thư viện visitor và liệt kê ứng viên. Hệ thống không tự gộp.
4. Mọi thao tác ghi của `traffic_admin` yêu cầu xác nhận và ghi audit.

### R7. Ảnh khuôn mặt (bật/tắt)

1. Cấu hình `--traffic-save-faces off|visitors|all`, mặc định `off`.
2. Khi bật, hệ thống lưu một ảnh khuôn mặt đã căn chỉnh 112×112 JPEG, chọn ảnh tốt nhất của mỗi chuyến.
3. Ảnh lưu dưới `recordings/trip_faces/YYYY-MM-DD/` (đã nằm trong `.gitignore`), quyền file `0600`. Đường dẫn ảnh không được ghi vào log.

### R8. Lưu trữ và thời hạn giữ dữ liệu

1. Dùng database riêng `db/traffic.db` (SQLite, WAL), tách khỏi `residents.db`.
2. Thời hạn giữ mặc định (cấu hình được):
   - Chuyến hành khách, chuyến cabin, điểm dừng: 730 ngày.
   - Embedding visitor: 90 ngày kể từ lần thấy cuối. Sau hạn, embedding bị xóa, `visitor_id` vẫn còn trong các chuyến như một số ẩn danh.
   - Ảnh khuôn mặt: 30 ngày, tổng dung lượng tối đa 5 GB.
3. Bảo vệ ổ đĩa: khi dung lượng trống dưới `min_free_mb` (mặc định 4096) thì ngừng lưu ảnh. Khi dưới mức tới hạn (mặc định 1024) thì ngừng ghi bản ghi mới và log error.
4. Việc dọn dữ liệu hết hạn chạy trên worker mỗi giờ, xóa theo lô nhỏ.
5. Export CSV theo khoảng thời gian cho từng bảng dữ liệu.

### R9. Không chặn vòng lặp xử lý frame

1. Ghi DB, mã hóa JPEG, so khớp visitor và dọn dữ liệu đều chạy trên worker. Hàng đợi có giới hạn, bỏ phần tử cũ nhất khi đầy, và đếm số phần tử bị bỏ.
2. Khi tắt app, các chuyến đang mở được đóng với cờ `shutdown`, hàng đợi được flush rồi worker được join.

### R10. Bảo mật và dữ liệu cá nhân

1. Log chỉ chứa ID số (`trip_key`, `resident_id`, `visitor_id`, tầng). Không log tên, `greeting_name`, căn hộ, embedding hay đường dẫn ảnh.
2. `db/traffic.db` chứa dữ liệu sinh trắc học: quyền `0600`, không commit, không gửi lên mạng trong v1.
3. Embedding và ảnh của người chưa đăng ký là dữ liệu sinh trắc học, thuộc nhóm dữ liệu cá nhân nhạy cảm theo Luật Bảo vệ dữ liệu cá nhân số 91/2025/QH15 (hiệu lực từ 01/01/2026). Trước khi bật R5 hoặc R7 ở production cần hoàn tất phần pháp lý (thông báo tại thang, căn cứ xử lý, thời hạn lưu). Đây là điều kiện vận hành, không phải việc của code.

### R11. Cấu hình

1. Thứ tự ưu tiên: CLI > giá trị mặc định khi biên dịch. `env.sh` truyền tham số theo `FACE_CABIN_ID`.
2. Tham số:
   - `--traffic-db PATH`: bật tính năng.
   - `--elevator-slave N`
   - `--traffic-visitor-reid 0|1`
   - `--traffic-save-faces off|visitors|all`
   - `--traffic-retention-days`, `--visitor-retention-days`, `--face-retention-days`
   - `--traffic-min-free-mb`
   - `--traffic-floor-min`, `--traffic-floor-max` (mặc định 0 và 9)
   - `--traffic-floor-tolerance-ms` (mặc định 6000), `--traffic-run-timeout-ms` (mặc định 60000), `--traffic-call-confirm-ms` (mặc định 15000)
   - `--traffic-cabin-capacity` (mặc định 15)
3. Tính năng cần occupancy đã bật. Nếu không có dữ liệu thanh ghi, chuyến hành khách vẫn được ghi với tầng để trống và cờ `floor_unknown`, kèm cảnh báo lúc khởi động.

### R12. Kiểm thử

- Parse thanh ghi, cập nhật một phần, giá trị sai, freshness.
- Máy trạng thái điểm dừng/chuyến chạy, gồm cả mất dữ liệu và chế độ đặc biệt.
- Lineage của occupancy qua ghost handoff, và slot khôi phục không có lineage.
- Ghép chuyến: thiếu Entry, thiếu Exit, cùng tầng, trễ thanh ghi, xung đột danh tính.
- So khớp visitor: khớp, tạo mới, không đủ mẫu, mẫu không nhất quán, thay mẫu khi đầy.
- Database: schema, insert, dọn dữ liệu, export, nâng visitor thành resident.
- Gọi tầng ngoài: đăng ký, được phục vụ bởi thang 2, được phục vụ bởi thang 1, bị xóa không có điểm dừng, mất dữ liệu giữa chừng, nhiều tầng cùng lúc, gắn vào chuyến hành khách.
- Gọi tầng trong cabin: đăng ký lúc cửa mở, đăng ký giữa chuyến chạy, phục vụ, hủy, nguồn `auto`/`button`, xác nhận lệnh gọi tự động.
- Replay file ghi mẫu `CONN_DATA` thật qua timeline: số chuyến chạy phát hiện bằng chênh lệch `8448`.
- Worker: hàng đợi tràn, bảo vệ ổ đĩa, flush khi tắt.
- CLI và `env.sh`.

### R13. Gọi tầng ngoài và thời gian chờ

1. Đọc `8215` (chiều lên) và `8225` (chiều xuống). Mỗi bit chuyển 0 → 1 là một lệnh gọi được đăng ký. Bit chuyển 1 → 0 là lệnh gọi được xóa.
2. Mỗi lệnh gọi ngoài ghi: khóa, tầng, chiều, giờ đăng ký, giờ xóa, điểm dừng đã phục vụ, thời gian chờ và kết quả.
   - Thời gian chờ = giờ cửa mở của điểm dừng phục vụ − giờ đăng ký.
   - Điểm dừng phục vụ là điểm dừng của thang 2 tại đúng tầng đó, có cửa mở trong khoảng dung sai quanh giờ xóa.
   - Kết quả: `served` (thang 2 phục vụ), `served_by_car1` (thang 1 dừng ở tầng đó trong khoảng dung sai), `cleared_without_stop` (bit tắt mà không thang nào dừng ở tầng đó), `register_gap`, `shutdown`.
   - Đăng ký và xóa lấy theo slave `-2`. Bản sao trên slave `-1` chỉ dùng để kiểm tra chéo.
3. Lệnh gọi đã bật sẵn khi app bắt đầu theo dõi (snapshot đầu tiên hoặc sau khi mất dữ liệu) được ghi với cờ `registration_unobserved`. Bản ghi này không dùng để tính thời gian chờ.
4. Chuyến hành khách vào ở một điểm dừng đã phục vụ lệnh gọi ngoài sẽ lưu khóa của lệnh gọi đó. Nếu điểm dừng phục vụ cả chiều lên và chiều xuống, chọn lệnh gọi cùng chiều với chuyến cabin kế tiếp.
5. Lệnh gọi ngoài không có người vào cabin vẫn được ghi (người gọi đã đi thang khác, đi bộ, hoặc gọi nhầm).
6. Thời gian chờ có sai số tới một chu kỳ gửi của gateway (khoảng ±5,6 s).

### R14. Gọi tầng trong cabin (OD theo điểm dừng)

Dữ liệu này không phụ thuộc camera, nên vẫn dùng được khi bộ đếm người bỏ sót. Trong phiên ghi mẫu 10 phút, bộ đếm chỉ ghi 2 Entry và 2 Exit, trong khi cabin có 12 chuyến chạy và ít nhất 8 tầng đích được bấm.

1. Mỗi bit của `8211` chuyển 0 → 1 là một lệnh gọi trong cabin được đăng ký. Bit chuyển 1 → 0 là lệnh được xóa.
2. Mỗi lệnh gọi trong cabin ghi: khóa, tầng đích, giờ đăng ký, điểm dừng lúc đăng ký (tầng đi), giờ xóa, điểm dừng đã phục vụ, kết quả và nguồn.
   - Điểm dừng lúc đăng ký: điểm dừng có `[giờ tới, giờ rời + dung sai]` chứa giờ đăng ký. Nếu lệnh được đăng ký giữa chuyến chạy, dùng điểm dừng trước đó và gắn cờ `registered_while_running`.
   - Kết quả: `served` (cabin dừng ở tầng đích có mở cửa), `cancelled` (bit tắt mà không dừng ở tầng đó), `register_gap`, `shutdown`.
   - Nguồn: `auto` nếu lệnh trùng với một lệnh gọi tự động đã gửi trong `--traffic-call-confirm-ms`; ngược lại là `button`.
3. Một lệnh gọi trong cabin là cận dưới của số người: nhiều người đi cùng tầng đích chỉ tạo một lệnh. Bản ghi không suy ra số người.
4. Chuyến hành khách có Exit được gắn với lệnh gọi trong cabin đã phục vụ ở cùng điểm dừng nếu có.

## Quyết định đã chốt

| # | Câu hỏi | Quyết định |
|---|---|---|
| 1 | Hậu tố `-2` có phải thang 2 không | Đúng. Mọi thanh ghi đọc ở slave `-2`. |
| 2 | Quy ước số tầng | Giữ quy ước của lệnh gọi tầng hiện có. Thang 2 phục vụ tầng B đến tầng 9. Chỉ số tầng = bit `n` = `8465 − 1`, B = 0 (xem kết quả ghi mẫu). |
| 3 | Ai xác minh danh tính | Người quản lý hoặc dev. V1 dùng CLI `traffic_admin` chạy trên thiết bị; không làm giao diện web hay API. |
| 4 | Gọi tầng ngoài trong v1 | Có (R13). |
| 5 | Ghi mẫu `CONN_DATA` chỉ đọc | Đồng ý. Kết quả ở phần dưới. |

## Kết quả ghi mẫu `CONN_DATA`

Phiên ghi chỉ đọc 10 phút, 14:49–14:59 ngày 03/11/2026 (giờ địa phương). Client chỉ gửi handshake, pong và `CONN_SUBSCRIBE`. Script và file dữ liệu nằm ở `.agents/tasks/elevator-traffic-log/` (không commit).

**Định dạng và chu kỳ**

- Message có dạng `42["CONN_DATA",{"sn","level","data":{...}}]`. Trong `data` có `ADDR` (slave), `TS` (giờ UTC của gateway) và các cặp `"<thanh ghi>-<slave>": giá trị`.
- Message đầu tiên là snapshot đầy đủ của cả hai slave. Các message sau chỉ chứa thanh ghi đã đổi, mỗi message cho một slave.
- 125 message trong 10 phút. Khoảng cách giữa hai message của cùng slave: nhỏ nhất 3,3 s, trung vị 5,6 s, lớn nhất 117 s (lúc thang đứng yên).
- Độ trễ từ `TS` của gateway tới lúc nhận: trung vị 0,01 s, lớn nhất 0,78 s. Sai số thời gian chủ yếu đến từ chu kỳ quét của gateway.

**Cách quy đổi tầng (đã chốt)**

- Bit `n` của `8211`, `8215`, `8225` ứng với giá trị `8465 = n + 1`. Kiểm chứng trên 14 lệnh gọi trong cabin (8 của thang 2, 6 của thang 1) và 11 lệnh gọi ngoài (5 do thang 2 phục vụ, 6 do thang 1): mỗi bit đều được xóa khi một cabin dừng ở `8465 = n + 1`, không có trường hợp lệch.
- Chỉ số tầng = bit `n` = `8465 − 1`. Như vậy B = 0 (`8465 = 1`), tầng 1 = 1 (`8465 = 2`), …, tầng 9 = 9 (`8465 = 10`).
- Chiều cao `8464` khớp với quy đổi này: `8465 = 2` ở 3,14 m (có đúng một điểm dừng thấp hơn), các tầng phía trên cách nhau khoảng 3,75 m. Gọi ngoài chiều lên ở bit 1 (sảnh tầng 1) xuất hiện nhiều nhất.
- Lệnh gọi tự động gửi `2^n` tới `8193-2` cho tầng `n`, cùng quy ước bit với `8211`. Chưa quan sát trực tiếp một lệnh gọi tự động bật bit `8211` vì trong phiên ghi không có cư dân nào được nhận ra. Việc này được kiểm chứng ở giai đoạn 0b.

**Ý nghĩa bit quan sát được (bổ sung cho tài liệu)**

| Thanh ghi | Quan sát |
|---|---|
| `8480` | bit0 lên, bit1 xuống (giữ nguyên khi đứng yên có hướng), bit2 đang chạy. Bit 8 và 10 luôn bật, chưa rõ ý nghĩa. |
| `8471` | bit3 cửa mở hết, bit4 cửa đóng hết. Bit7 xuất hiện ngay sau khi tới tầng, chưa rõ ý nghĩa; lưu giá trị thô. |
| `8472` | bit5 lệnh mở cửa, bit6 lệnh đóng cửa, bit12 chạy lên, bit13 chạy xuống. |
| `8467` | bit14 bằng tầng, bật khi đứng yên và tắt khi chạy. |
| `8448` | Tăng 1 mỗi lần cabin rời điểm dừng. Phát hiện chuyến chạy theo bit2 của `8480` cho 12 chuyến, khớp đúng chênh lệch 12 của bộ đếm. |
| `8215`/`8225` | Giống nhau trên slave `-1` và `-2` (nhóm thang). Ví dụ lúc 14:54:16 có gọi ngoài lên và xuống ở cùng tầng 5; thang 1 phục vụ cả hai. |

**Phát hiện ảnh hưởng tới thiết kế**

1. Không thể dùng quy tắc freshness 5 s vì `CONN_DATA` chỉ gửi khi giá trị đổi (R1.2 đã sửa).
2. Bộ đếm người production đang ở count = 275 (`Uncertain`) và bỏ sót phần lớn lượt vào/ra (R14 bổ sung nguồn OD không phụ thuộc camera).
3. Bộ lập lịch adaptive inference hiện có dùng cùng quy tắc 5 s cho thanh ghi cửa. Ngày 03/11 có 745 lần chuyển sang `DEGRADED door_fresh=0`. Lúc 14:55:09 cửa mở ở tầng 1 trong khi bộ lập lịch đang `DEGRADED`, và người vào lúc đó không tạo Entry. Đây là giả thuyết về một nguyên nhân bỏ sót, chưa được kiểm chứng. Việc này thuộc spec `door-register-inference`/`cabin-occupancy`, không nằm trong spec này.
