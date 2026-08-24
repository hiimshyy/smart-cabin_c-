# Bulk Enroll CSV Format — Smart Elevator Cabin

Tool `bulk_enroll` sẽ đọc file CSV chứa danh sách cư dân + ảnh thẻ 3×4,
detect face → extract embedding → import vào SQLite DB.

## File CSV format

Encoding **UTF-8**, phân cách bằng **dấu phẩy**, dòng đầu là header.

### Cột (theo thứ tự)

| # | Cột | Kiểu | Bắt buộc | Ghi chú |
|---|-----|------|----------|---------|
| 1 | `name` | text | ✅ | Họ tên đầy đủ ("Nguyễn Thị Nga") |
| 2 | `apartment` | text | ✅ | Số căn hộ ("12A05") |
| 3 | `home_floor` | int | ✅ | Tầng nhà (1-N) |
| 4 | `language` | `vi`/`en` | ⭕ | Default `vi` |
| 5 | `greeting_name` | text | ⭕ | Cách chào ("bác Nga", "Mr. Smith"). Default = `name` |
| 6 | `role` | text | ⭕ | `resident`/`staff`/`vip`/`guest_regular`, default `resident` |
| 7 | `id_photo` | filename | ✅ | Tên file ảnh trong `--photos-dir` |
| 8 | `notes` | text | ⭕ | Ghi chú tự do |

### Ví dụ

```csv
name,apartment,home_floor,language,greeting_name,role,id_photo,notes
Nguyễn Thị Nga,12A05,12,vi,bác Nga,resident,nga.jpg,
John Smith,08B03,8,en,Mr. Smith,resident,smith.jpg,foreign resident
Trần Văn Nam,05C01,5,vi,anh Nam,resident,nam.jpg,
Lê Thị Hoa,15D02,15,vi,cô Hoa,vip,hoa.jpg,building manager
Bảo vệ tòa nhà,,1,vi,bảo vệ,staff,baove.jpg,gets floor 1 by default
```

## Chạy import

```bash
./bulk_enroll \
    --csv residents.csv \
    --photos-dir ./id_photos \
    --db db/residents.db \
    --det-model model/Retinaface_resnet50_320_uint8_a733.nb \
    --recog-model model/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 --recog-bgr
```

## Xử lý lỗi

Tool sẽ tạo `enroll_failures.csv` liệt kê các row skip với lý do:

| Lý do | Cách xử lý |
|---|---|
| `photo_not_found` | File `id_photo` không tồn tại trong `--photos-dir` |
| `no_face_detected` | Detect fail — ảnh quá bé/tối/lệch |
| `multi_face_detected` | Ảnh có nhiều mặt (dùng cái lớn nhất, cảnh báo) |
| `face_too_small` | Bbox < 60px — ảnh scan chất lượng thấp |
| `duplicate_apartment` | Căn hộ đã có resident khác cùng name/floor |
| `invalid_row` | Thiếu field bắt buộc |

HR nhận `enroll_failures.csv` → chụp/scan lại ảnh chất lượng tốt hơn → chạy lại tool với subset.

## Behavior chi tiết

1. **Nếu resident đã tồn tại** (match theo `name + apartment`):
   - Default: **cập nhật** thông tin (soft update)
   - `--overwrite`: xóa embeddings cũ, add mới hoàn toàn
   - `--skip-existing`: bỏ qua

2. **Mỗi ảnh 3×4 sinh 1 embedding** vào bảng `embeddings` với `source='id_photo'`.
3. **Multi-embedding self-supervised** sẽ được add từ live cabin (Week 2+).

4. **Threshold enrollment nới lỏng** (0.30) cho 2 tuần đầu, siết lại 0.40 sau khi đủ in-cabin embeddings.
