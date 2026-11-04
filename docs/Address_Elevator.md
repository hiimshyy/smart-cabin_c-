| **Địa chỉ** **(16)** | **Địa chỉ**  **trên Cloud****(10)** | **Địa chỉ biến tần** | **Ý nghĩa** | **Diễn giải** | **Cơ số** | **Scale** | **Đơn vị** |
| --- | --- | --- | --- | --- | --- | --- | --- |
| *0x2013* | *8211-1* | *U0.13* | Đăng kí gọi tầng trong cabin ( tầng 1~16) **Inside call** | 000000**1**00**1**00000 Bit 0: tầng 1 Bit 1: tầng 2 …… Bit 15: tầng 16 **Gọi tầng nào thì bit tương ứng được set lên 1** | 16 |  |  |
| *0x2017* | *8215-1* | *U0.17* | Đăng kí gọi tầng chiều lên (tầng 1~16) **Up call** | 000000100100000 Bit 0: tầng 1 Bit 1: tầng 2 …… Bit 15: tầng 16 **Gọi tầng nào thì bit tương ứng được set lên 1** | 16 |  |  |
| *0x2021* | *8225-1* | *U0.21* | Đăng kí gọi tầng chiều xuống (tầng 1~16) **Down call** | 000000100100000 Bit 0: tầng 1 Bit 1: tầng 2 …… Bit 15: tầng 16 **Gọi tầng nào thì bit tương ứng được set lên 1** | 16 |  |  |
| *0x2100* | *8448-1* | *U1.00* | Tổng số lần chạy (bit thấp) **Run number** |  | 10 |  |  |
| *0x2101* | *8449-1* | *U1.01* | Tổng số lần chạy (bit cao) **Run number** |  | 10 |  |  |
| *0x2110* | *8464-1* | *U1.10* | Chiều cao hiện tại của cabin **Height** |  | 10 | /100 | m |
| *0x2111* | *8465-1* | *U1.11* | Tầng hiện tại | Dùng cho hiển thị tầng hiện tại ở trong cabin | 10 |  |  |
| *0x2113* | *8467-1* | *U1.13* | Trạng thái bằng tầng **Area** | 1**1**00000100110000 Bit 14: tín hiệu bằng tầng **Khi bằng tầng thì bit được set lên 1** | 16 |  |  |
| *0x2114* | *8468-1* | *U1.14* | Đầu vào điện áp cao | **111**0000001 Bit 7: mạch an toàn (**Safety Loop**) Bit 8: mạch cửa tầng (**Hall Lock**) Bit 9: mạch cửa car (**Car door lock**) **Tín hiệu nào được kết nối thì bit tương ứng được set lên 1** | 16 |  |  |
| *0x2117* | *8471-1* | *U1.17* | Đầu vào board CTB | 0**11**00000000**111**00 Bit 2: tín hiệu quá tải (**Overload**) Bit 3: tín hiệu giới hạn mở cửa (cửa trước)           (**Open door in place**) Bit 4: tín hiệu giới hạn đóng cửa (cửa trước)           (**Close in place**) Bit 13: tín hiệu giới hạn mở cửa (cửa sau) Bit 14: tín hiệu giới hạn đóng cửa (cửa sau) | 16 |  |  |
| *0x2118* | *8472-1* | *U1.18* | Trạng thái đầu ra 12 bit | 0000**1111**0000 Bit 5: tín hiệu mở cửa cabin (cửa trước)          (**Open door output**) Bit 6: tín hiệu đóng cửa cabin (cửa trước)          (**Closed output**) Bit 7: tín hiệu mở cửa cabin (cửa sau) Bit 8: tín hiệu đóng cửa cabin (cửa sau) | 16 |  |  |
| *0x2119* | *8473-1* | *U1.19* | Tốc độ chạy hiện tại **Running Speed** |  | 10 |  | mm/s |
| *0x2120* | *8480-1* | *U1.20* | Trạng thái hoạt động | 0000**11**00**111**0**1111** bit 0: hiển thị thang chạy lên (**Mũi tên chạy chiều lên**) bit 1: hiển thị thang chạy xuống (**Mũi tên chạy chiều xuống**) bit 2: hiển thị thang đang chạy bit 3: hiển thị bảo trì (**Maintenance**) bit 5: hiển thị thang đang lỗi (**Fault**) bit 6: hiển thị thang máy đang khóa Parking          (**Lock ladder**) bit 7: hiển thị báo cháy (**Fire fighting**) bit 11: hiển thị chế độ độc lập trong cabin            (**Independent State**) | 16 |  |  |
| *0x2121* | *8481-1* | *U1.21* | Dòng điện hiện tại | Làm thêm giao diện giám sát | 10 |  | A |
| *0x2122* | *8482-1* | *U1.22* | Điện áp hiện tại |  | 10 |  | V |
| *0x3000* | *12288-1* | *E0.00* | Tổng số lỗi và số lỗi hiện tại | Đếm được tổng số lỗi: 00000000 **00000000** 8 bit đầu tiên sẽ là đếm tổng số lỗi 8 bit tiếp theo là số lỗi hiện tại |  |  |  |
| *`* | *12289-1* | *E0.01* | Lỗi thứ 1 |  | 10 |  |  |
| *0x3002* | *12290-1* | *E0.02* | Mã lỗi thứ 1 |  | 10 |  |  |
| *0x3003* | *12291-1* | *E0.03* | Năm xảy ra lỗi thứ 1 | ![image1.png](Address_Elevator_images/image1.png) | 16 |  |  |
| *0x3004* | *12292-1* | *E0.04* | Tháng và ngày xảy ra lỗi thứ 1 | ![image2.png](Address_Elevator_images/image2.png) | 16 |  |  |
| *0x3005* | *12293-1* | *E0.05* | Giờ và phút xảy ra lỗi thứ 1 |  | 16 |  |  |
| *0x3006* | *12294-1* | *E0.06* | Giây xảy ra lỗi thứ 1 |  | 16 |  |  |
| *0x3007* | *12295-1* | *E0.07* | Tầng hiện tại xảy ra lỗi thứ 1 |  | 10 |  |  |
| *0x3008* | *12296-1* | *E0.08* | Input state 1 | ![image3.png](Address_Elevator_images/image3.png) | 16 |  |  |
| *0x3009* | *12297-1* | *E0.09* | Input State 2 | ![image4.png](Address_Elevator_images/image4.png) | 16 |  |  |
| *0x3010* | *12304-1* | *E0.10* | Output state | ![image5.png](Address_Elevator_images/image5.png) | 16 |  |  |
| *0x3011* | *12305-1* | *E0.11* | Tốc độ thang máy tại lỗi thứ 1 | ![image5.png](Address_Elevator_images/image5.png) | 10 |  | mm/s |
| *0x3012* | *12306-1* | *E0.12* | Vị trí thang máy tại lỗi thứ 1 | ![image5.png](Address_Elevator_images/image5.png) | 10 | /100 | m |
| *0x3013* | *12307-1* | *E0.13* | Điện áp DC BUS tại lỗi thứ 1 | ![image6.png](Address_Elevator_images/image6.png) | 10 | /10 | V |
| *0x3014* | *1230**8**-1* | *E0.14* | Dòng điện tại lỗi thứ 1 |  | 10 |  | A |
| *0x3015* | *1230**9**-1* | *E0.15* | Tần số khi xảy ra lỗi 1 |  | 10 | /10 | Hz |
| *0x3020* | *12320-1* |  | Tổng số lỗi và số lỗi hiện tại | Đếm được tổng số lỗi: 00000000 **00000000** 8 bit đầu tiên sẽ là đếm tổng số lỗi 8 bit tiếp theo là số lỗi hiện tại |  |  |  |
| *0x3021* | *12321-1* |  | Lỗi thứ 2 |  | 10 |  |  |
| *0x3022* | *12322-1* |  | Mã lỗi thứ 2 |  | 10 |  |  |
| *0x3023* | *12323-1* |  | Năm xảy ra lỗi thứ 2 | ![image1.png](Address_Elevator_images/image1.png) | 16 |  |  |
| *0x3024* | *12324-1* |  | Tháng và ngày xảy ra lỗi thứ 2 | ![image2.png](Address_Elevator_images/image2.png) | 16 |  |  |
| *0x3025* | *12325-1* |  | Giờ và phút xảy ra lỗi thứ 2 |  | 16 |  |  |
| *0x3026* | *12326-1* |  | Giây xảy ra lỗi thứ 2 |  | 16 |  |  |
| *0x3027* | *12327-1* |  | Tầng hiện tại xảy ra lỗi thứ 2 |  | 16 |  |  |
| *0x3028* | *12328-1* |  | Input state 1 | ![image3.png](Address_Elevator_images/image3.png) | 16 |  |  |
| *0x3029* | *12329-1* |  | Input State 2 | ![image4.png](Address_Elevator_images/image4.png) | 16 |  |  |
| *0x3030* | *12336-1* |  | Output state | ![image5.png](Address_Elevator_images/image5.png) | 16 |  |  |
| *0x3031* | *12337-1* |  | Tốc độ thang máy tại lỗi thứ 2 | ![image5.png](Address_Elevator_images/image5.png) | 10 |  | mm/s |
| *0x3032* | *12338-1* |  | Vị trí thang máy tại lỗi thứ 2 | ![image5.png](Address_Elevator_images/image5.png) | 10 | /100 | m |
| *0x3033* | *12339-1* |  | Điện áp DC BUS tại lỗi thứ 2 | ![image6.png](Address_Elevator_images/image6.png) | 10 | /10 | V |
| *0x3034* | *12340-1* |  | Dòng điện tại lỗi thứ 2 | ![image6.png](Address_Elevator_images/image6.png) | 10 |  | A |
| *0x3035* | *12341-1* |  | Tần số khi xảy ra lỗi 2 | ![image6.png](Address_Elevator_images/image6.png) | 10 | /10 | Hz |
| *0x3041* | *12353-1* |  | Lỗi thứ 3 |  | 10 |  |  |
| *0x3042* | *12354-1* |  | Mã lỗi thứ 3 |  | 10 |  |  |
| *0x3043* | *12355-1* |  | Năm xảy ra lỗi thứ 3 |  | 16 |  |  |
| *0x3044* | *12356-1* |  | Tháng và ngày xảy ra lỗi thứ 3 |  | 16 |  |  |
| *0x3045* | *12357-1* |  | Giờ và phút xảy ra lỗi thứ 3 |  | 16 |  |  |
| *0x3046* | *12358-1* |  | Giây xảy ra lỗi thứ 3 |  | 16 |  |  |
| *0x3047* | *12359-1* |  | Tầng hiện tại xảy ra lỗi thứ 3 |  | 10 |  |  |
| *0x3048* | *12360-1* |  | Input state 1 |  | 16 |  |  |
| *0x3049* | *12361-1* |  | Input State 2 |  | 16 |  |  |
| *0x3050* | *12368-1* |  | Output state |  | 16 |  |  |
| *0x3051* | *12369-1* |  | Tốc độ thang máy tại lỗi thứ 3 |  |  |  |  |
| *0x3052* | *12370-1* |  | Vị trí thang máy tại lỗi thứ 3 |  |  |  |  |
| *0x3053* | *12371-1* |  | Điện áp DC BUS tại lỗi thứ 3 |  |  |  |  |
| *0x3054* | *12372-1* |  | Dòng điện tại lỗi thứ 3 |  |  |  |  |
| *0x3055* | *12373-1* |  | Tần số khi xảy ra lỗi 3 |  |  |  |  |
| *0x3061* | *12385-1* |  | Lỗi thứ 4 |  |  |  |  |
| *0x3062* | *12386-1* |  | Mã lỗi thứ 4 |  |  |  |  |
| *0x3063* | *12387-1* |  | Năm xảy ra lỗi thứ 4 |  |  |  |  |
| *0x3064* | *12388-1* |  | Tháng và ngày xảy ra lỗi thứ 4 |  |  |  |  |
| *0x3065* | *12389-1* |  | Giờ và phút xảy ra lỗi thứ 4 |  |  |  |  |
| *0x3066* | *12390-1* |  | Giây xảy ra lỗi thứ 4 |  |  |  |  |
| *0x3067* | *12391-1* |  | Tầng hiện tại xảy ra lỗi thứ 4 |  |  |  |  |
| *0x3068* | *12392-1* |  | Input state 1 |  |  |  |  |
| *0x3069* | *12393-1* |  | Input State 2 |  |  |  |  |
| *0x3070* | *12400-1* |  | Output state |  |  |  |  |
| *0x3071* | *12401-1* |  | Tốc độ thang máy tại lỗi thứ 4 |  |  |  |  |
| *0x3072* | *12402-1* |  | Vị trí thang máy tại lỗi thứ 4 |  |  |  |  |
| *0x3073* | *12403-1* |  | Điện áp DC BUS tại lỗi thứ 4 |  |  |  |  |
| *0x3074* | *12404-1* |  | Dòng điện tại lỗi thứ 4 |  |  |  |  |
| *0x3075* | *12405-1* |  | Tần số khi xảy ra lỗi 4 |  |  |  |  |
| *0x3081* | *12417-1* |  | Lỗi thứ 5 |  |  |  |  |
| *0x3082* | *12418-1* |  | Mã lỗi thứ 5 |  |  |  |  |
| *0x3083* | *12419-1* |  | Năm xảy ra lỗi thứ 5 |  |  |  |  |
| *0x3084* | *12420-1* |  | Tháng và ngày xảy ra lỗi thứ 5 |  |  |  |  |
| *0x3085* | *12421-1* |  | Giờ và phút xảy ra lỗi thứ 5 |  |  |  |  |
| *0x3086* | *12422-1* |  | Giây xảy ra lỗi thứ 5 |  |  |  |  |
| *0x3087* | *12423-1* |  | Tầng hiện tại xảy ra lỗi thứ 5 |  |  |  |  |
| *0x3088* | *12424-1* |  | Input state 1 |  |  |  |  |
| *0x3089* | *12425-1* |  | Input State 2 |  |  |  |  |
| *0x3090* | *12432-1* |  | Output state |  |  |  |  |
| *0x3091* | *12433-1* |  | Tốc độ thang máy tại lỗi thứ 5 |  |  |  |  |
| *0x3092* | *12434-1* |  | Vị trí thang máy tại lỗi thứ 5 |  |  |  |  |
| *0x3093* | *12435-1* |  | Điện áp DC BUS tại lỗi thứ 5 |  |  |  |  |
| *0x3094* | *12436-1* |  | Dòng điện tại lỗi thứ 5 |  |  |  |  |
| *0x3095* | *12437-1* |  | Tần số khi xảy ra lỗi 5 |  |  |  |  |
| 0x3101 |  |  | Lỗi thứ 6 |  |  |  |  |
| 0x3102 |  |  | Mã lỗi thứ 6 |  |  |  |  |
| 0x3103 |  |  | Năm xảy ra lỗi thứ 6 |  |  |  |  |
| 0x3104 |  |  | Tháng và ngày xảy ra lỗi thứ 6 |  |  |  |  |
| 0x3105 |  |  | Giờ và phút xảy ra lỗi thứ 6 |  |  |  |  |
| 0x3106 |  |  | Giây xảy ra lỗi thứ 6 |  |  |  |  |
| 0x3107 |  |  | Tầng hiện tại xảy ra lỗi thứ 6 |  |  |  |  |
| 0x3108 |  |  | Input state 1 |  |  |  |  |
| 0x3109 |  |  | Input State 2 |  |  |  |  |
| 0x3110 |  |  | Output state |  |  |  |  |
| 0x3111 |  |  | Tốc độ thang máy tại lỗi thứ 6 |  |  |  |  |
| 0x3112 |  |  | Vị trí thang máy tại lỗi thứ 6 |  |  |  |  |
| 0x3113 |  |  | Điện áp DC BUS tại lỗi thứ 6 |  |  |  |  |
| 0x3114 |  |  | Dòng điện tại lỗi thứ 6 |  |  |  |  |
| 0x3115 |  |  | Tần số khi xảy ra lỗi 6 |  |  |  |  |
| … |  | … | … |  |  |  |  |
| … |  | … | … |  |  |  |  |
| … |  | … | … |  |  |  |  |

- Lưu ý:  hệ thống lưu được lịch sử 30 lỗi:
- Lỗi thứ 1: các chỉ số được lưu từ địa chỉ: 0x3001~0x3015
- Lỗi thứ 2: các chỉ số được lưu từ địa chỉ: 0x3021~0x3035
- Lỗi thứ 3: các chỉ số được lưu từ địa chỉ: 0x3041~0x3055
- …
- Lỗi thứ 30: các chỉ số được lưu từ địa chỉ: 0x3581~0x3595
- **Khi có thêm lỗi thứ 31 xuất hiện, hệ thống sẽ tự ghi đè lên** **lỗi thứ 1.**

**Giao diện tham khảo:**
![image7.png](Address_Elevator_images/image7.png)
