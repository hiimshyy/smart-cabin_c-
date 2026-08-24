# RetinaFace + MobileFaceNet — Orange Pi A733 NPU

Realtime face detection + recognition demo chạy trên Orange Pi A733 (NPU v3).
Pipeline: USB camera → RetinaFace (320×320) → align 5-landmarks → MobileFaceNet (112×112) → cosine match vs database.

## Kết quả benchmark

| Chỉ số | Detect-only | Detect + Recognize |
|---|---|---|
| End-to-end FPS | 14.65 | **13.86** |
| Detect NPU latency | 19.24 ms | 19.80 ms |
| Recog NPU latency | — | **2.96 ms / face** |
| Capture MJPG (bottleneck) | 38.5 ms | 36.0 ms |
| E2E total | 62.0 ms | 62.1 ms |

Same-person cosine similarity: **0.75 – 0.83**. Recommended `--match-thr = 0.35`.

## Cấu trúc thư mục

```
retinaface_demo_linux_a733/
├── README.md                     # docs (file này)
├── Makefile                      # build system
├── .gitignore
│
├── src/                          # source code
│   ├── main.cpp                  # realtime camera app (detect [+ recognize])
│   ├── enroll_faces.cpp          # offline: folder ảnh → .fdb DB
│   ├── capture_person.cpp        # chụp N ảnh 1 người từ camera
│   ├── add_person.cpp            # thêm 1 người vào .fdb có sẵn từ ảnh
│   ├── retinaface_pre.{h,cpp}    # letterbox 640×480 → 320×320 HWC BGR uint8
│   ├── retinaface_post.{h,cpp}   # softmax + decode bbox/landmarks + NMS
│   ├── anchors.{h,cpp}           # sinh 4200 priors cho RetinaFace 320
│   ├── face_align.{h,cpp}        # affine 5-lmk → 112×112 (ArcFace canonical)
│   ├── face_recog.{h,cpp}        # NPU context #2 → embedding 512-D L2-norm
│   ├── face_db.{h,cpp}           # .fdb binary DB + cosine match top-1
│   └── log/log.h                 # mute awnn verbose log
│
├── model/                        # NPU models (.nb)
│   ├── Retinaface_resnet50_320_uint8_a733.nb   # detection (19 MB)
│   └── w600k_mbf_uint8_a733.nb                 # recognition (2.7 MB)
│
├── faces/                        # ảnh enroll: <name>/frame_*.jpg
├── build/                        # object files (Makefile tự tạo, .gitignore)
│
├── legacy/                       # tài liệu cũ & vendor reference
│   ├── PLAN.md
│   ├── retinaface_demo_a733      # vendor binary reference
│   ├── out_retinaface.png        # vendor sample output
│   └── images/                   # test frames cũ
│
├── face_recog_app             # binary (compile ra ở root — tiện ./run)
├── enroll_faces, capture_person, add_person
│
├── db/                           # database files
│   ├── faces_all.fdb
│   └── faces_*.fdb
│
└── env.sh                        # source vào để có shortcut face_run, face_add, ...
```

## Cách chạy nhanh (khuyến nghị) — dùng `env.sh`

```bash
# 1 lần mỗi session (hoặc thêm vào ~/.bashrc):
source env.sh

# Sau đó có 7 lệnh ngắn gọn:
face_help                          # in cheat sheet
face_run                           # chạy realtime với DB mặc định
face_capture alice 5               # chụp 5 frames của alice
face_add bob /path/bob.jpg         # thêm bob từ ảnh
face_add alice new.jpg --merge     # trộn ảnh mới với embedding cũ
face_enroll                        # rebuild toàn bộ DB từ faces/
face_bench 100                     # bench 100 frames + summary
face_ls                            # liệt kê DB + folders enroll
```

Áp vĩnh viễn:
```bash
echo "source $(pwd)/env.sh" >> ~/.bashrc
```

Sau khi cài, chỉ cần `face_run` là chạy — không phải nhớ 4 argument dài.

## Build

```bash
make -j4          # → face_recog_app, enroll_faces, capture_person, add_person
make clean        # xóa build/ và 4 binary
```

Yêu cầu:
- Orange Pi A733 với NPU driver (VIPLite 2.0.3.2+)
- OpenCV 4.6 (`pkg-config opencv4`)
- AI SDK ở `/home/orangepi/ai-sdk` (chứa `awnn_lib.c`, `awnn_quantize.c`)
- `libVIPhal`, `libNBGlinker` trên hệ thống

## Workflow đầy đủ — từ 0 lên hệ thống 1000 người

### Bước 1 — Chụp ảnh mỗi người

Cho mỗi người (chạy 1 lệnh cho mỗi người):

```bash
./capture_person --name alice --count 5
```

- Mở cửa sổ preview với overlay bbox màu xanh khi detect OK
- Auto-chụp 5 frames cách nhau 500ms khi phát hiện đúng 1 khuôn mặt to (>= 100px)
- Bấm SPACE để chụp thủ công, Q/ESC để thoát

Options:
```
--name X            Tên định danh (bắt buộc)
--out DIR           Thư mục gốc (default: faces)
--count N           Số frames cần chụp (default: 5)
--cam N             /dev/videoN (default: 0)
--min-face-px N     Khuôn mặt bé hơn thì skip (default: 100)
--interval-ms N     Khoảng cách tối thiểu giữa các shot (default: 500)
--no-preview        Chạy headless, không mở cửa sổ
```

Kết quả: `faces/alice/frame_01.jpg`, `frame_02.jpg`, ...

Với 1000 người: mỗi người mất ~5-10s → tổng ~2-3 giờ chụp. Có thể script hóa với queue tên.

### Bước 2 — Build database

Sau khi đã có `faces/<name1>/`, `faces/<name2>/`, …:

```bash
./enroll_faces \
    --dir faces \
    --out faces.fdb \
    --det-model model/Retinaface_resnet50_320_uint8_a733.nb \
    --recog-model model/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 \
    --recog-bgr
```

- Duyệt mỗi subfolder → 1 identity
- Với mỗi ảnh: detect largest face → align 112 → embedding
- Trung bình embeddings của cùng 1 người → 1 vector L2-norm/identity
- Save ra `faces.fdb` (~2 KB/identity)

Log ví dụ:
```
[enroll] persons=1000, imgs_scanned=5000, imgs_used=4987
[enroll] saved 1000 identities → faces.fdb
```

DB size cho 1000 identities: ~2 MB. Rebuild từ đầu khi cần thêm/bớt người (nhanh, ~15 ms/ảnh trên A733).

### Bước 2b (tùy chọn) — Thêm 1 người vào DB có sẵn (không rebuild)

Thay vì rebuild toàn bộ, dùng `add_person` để append 1 identity:

```bash
# Thêm mới từ 1 ảnh
./add_person \
    --name john \
    --image /path/to/john.jpg \
    --db faces.fdb \
    --det-model model/Retinaface_resnet50_320_uint8_a733.nb \
    --recog-model model/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 --recog-bgr

# Thêm mới từ nhiều ảnh (chất lượng cao hơn)
./add_person --name jane \
    --image jane1.jpg --image jane2.jpg --image jane3.jpg \
    --db faces.fdb \
    --det-model model/Retinaface_resnet50_320_uint8_a733.nb \
    --recog-model model/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 --recog-bgr

# Nếu tên đã có trong DB:
#   (default) → báo lỗi, không thay đổi
#   --replace → xóa entry cũ, thêm mới
#   --merge   → trung bình embedding cũ với ảnh mới
```

Latency: ~250ms lần đầu (init NPU), ~20-30ms mỗi ảnh sau đó. Nhanh hơn rebuild toàn bộ 1000 người (~75s).

### Bước 3 — Chạy realtime match

```bash
./face_recog_app \
    model/Retinaface_resnet50_320_uint8_a733.nb \
    --recog-model model/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 \
    --recog-bgr \
    --face-db faces.fdb \
    --match-thr 0.35
```

Trên khung mặt hiển thị `<name> <score>` (ví dụ `alice 0.78`). Score dưới `--match-thr` → hiển thị `unknown`.

Options:
```
detect.nb                Đường dẫn RetinaFace .nb (positional, default nếu bỏ qua)
cam_id                   /dev/videoN (positional, default: 0)
--frames N               Chạy N frames rồi in bench summary và thoát
--recog-model PATH       Bật recognition, path .nb
--recog-dim N            Embedding dim (default: 512)
--recog-bgr              Feed BGR trực tiếp (dùng khi convert với reverse_channel: true)
--face-db PATH           Load .fdb — không có DB thì chỉ show embedding size
--match-thr F            Threshold cosine similarity (default: 0.35)
```

Không có `--recog-model` → chạy như detect-only (backward compatible).

## Tuning `--match-thr`

Khuyến nghị dựa trên kết quả benchmark thực tế:

| Ngưỡng | Behavior |
|---|---|
| 0.20 | Cực kỳ dễ dãi — dễ nhầm người |
| 0.35 (default) | An toàn cân bằng, self-match 0.75+ luôn pass |
| 0.45 | Chặt — chỉ match khi rất giống (giảm false positive) |
| 0.55 | Rất chặt — có thể miss same-person do lighting |

Cách tinh chỉnh cho hệ thống thực:
1. Chụp 5-10 ảnh khác nhau (góc, ánh sáng) của cùng 1 người vào 1 subfolder
2. Chạy `face_recog_app` với `--match-thr 0.10` — quan sát score self-match
3. Chọn `--match-thr = (min self-score) - 0.05`
4. Test với người lạ — score nên < 0.30

## Model conversion (PC-side, đã hoàn tất)

Đã convert `.nb` từ ONNX bằng Acuity Toolkit trên PC Linux. Model output:
- `Retinaface_resnet50_320_uint8_a733.nb` — RetinaFace ResNet50, input 320×320 uint8
- `w600k_mbf_uint8_a733.nb` — MobileFaceNet 512-D (từ InsightFace buffalo_s bundle)

Preprocess của MobileFaceNet: mean=127.5, scale=1/127.5, reverse_channel=true (nên board dùng flag `--recog-bgr`).

## Định dạng file `.fdb`

Little-endian binary:
```
offset  size  content
0       4     magic "FDB1"
4       4     int32 dim (thường = 512)
8       4     int32 count
12      ..    entries:
              uint16 name_len
              char[name_len] name (UTF-8)
              float32[dim] embedding (đã L2-normalized)
```

## Vấn đề đã biết & tương lai

- **MJPG capture bottleneck 38ms**: chiếm 62% latency. Có thể tách capture thread riêng để đạt ~26 FPS lý thuyết. Chưa triển khai.
- **NPU chỉ có 1 core**: detect + recognize sharing → không parallel được. Recognition đã rất nhanh (3ms) nên không phải bottleneck.
- **Không có drift/liveness**: chỉ dùng embedding cosine. Không phòng chống spoofing bằng ảnh in.
- **ArcFace R50 chưa test**: có thể convert thêm cho use case cần accuracy cao hơn (LFW 99.82% vs 99.28% MobileFaceNet), trade-off latency 25-35ms/face.
