# SCRFD + MobileFaceNet — Orange Pi A733 NPU

Realtime face detection + recognition chạy trên Orange Pi A733 (NPU v3).
Pipeline: USB / RTSP camera → **SCRFD 2.5g_bnkps 640** → align 5-landmarks → **MobileFaceNet 112** → cosine match vs database `.fdb`.

## Kết quả benchmark (USB webcam)

| Chỉ số | SCRFD + Recognition |
|---|---|
| End-to-end FPS | **~25 FPS** |
| Detect NPU latency | **~10-17 ms** |
| Recog NPU latency | ~3 ms / face |
| Postproc (16800 anchors) | 0.1-0.2 ms |
| Preprocess (letterbox 640) | ~1 ms |
| Capture MJPG (bottleneck) | 15-19 ms |

Same-person cosine similarity: **~0.79-0.80**. Recommended `--match-thr = 0.35`.

Model size: **947 KB** (nhẹ hơn ArcFace ResNet50 ~20×).

## Cấu trúc thư mục

```
face_recog_app/
├── README.md
├── Makefile
├── env.sh                        # source vào để có shortcut face_run, ...
├── .gitignore
│
├── src/
│   ├── main.cpp                  # realtime app (detect + recognize)
│   ├── enroll_faces.cpp          # offline: folder ảnh → .fdb DB
│   ├── capture_person.cpp        # chụp N ảnh 1 người từ camera
│   ├── add_person.cpp            # thêm/update 1 người vào DB có sẵn
│   ├── detect_pre.{h,cpp}        # letterbox → 640×640 HWC BGR uint8
│   ├── detection.h               # struct Detection (shared)
│   ├── scrfd_post.{h,cpp}        # SCRFD 9-output decode + NMS
│   ├── face_align.{h,cpp}        # affine 5-lmk → 112×112 (ArcFace canonical)
│   ├── face_recog.{h,cpp}        # NPU context → embedding 512-D L2-norm
│   ├── face_db.{h,cpp}           # .fdb binary DB + cosine match top-1
│   └── log/log.h                 # mute awnn verbose log
│
├── model/
│   ├── face_det/
│   │   └── scrfd_2.5g_bnkps640_uint8_a733.nb   # detection (947 KB)
│   └── face_recog/
│       └── w600k_mbf_uint8_a733.nb             # recognition (2.7 MB)
│
├── faces/                        # ảnh enroll: <name>/frame_*.jpg
├── build/                        # object files (.gitignore)
├── db/                           # database files (.fdb)
│
├── face_recog_app                # binary compile ra ở root
├── enroll_faces, capture_person, add_person
│
└── legacy/                       # snapshots + tài liệu cũ
```

## Cách chạy nhanh — dùng `env.sh`

```bash
# 1 lần mỗi session (hoặc thêm vào ~/.bashrc):
source env.sh

# Sau đó có 9 lệnh ngắn gọn:
face_help                          # in cheat sheet
face_run                           # chạy realtime với DB mặc định
face_run_rtsp URL                  # chạy realtime với RTSP camera
face_capture alice 5               # chụp 5 frames của alice
face_add bob /path/bob.jpg         # thêm bob từ ảnh
face_add alice new.jpg --merge     # trộn ảnh mới với embedding cũ
face_enroll                        # rebuild toàn bộ DB từ faces/
face_bench 100                     # bench 100 frames + summary
face_bench_rtsp URL 200            # bench trên RTSP
face_ls                            # liệt kê DB + folders enroll
```

Áp vĩnh viễn:
```bash
echo "source $(pwd)/env.sh" >> ~/.bashrc
```

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
- GStreamer plugins (rtsp) nếu dùng RTSP source

## Workflow đầy đủ — từ 0 lên hệ thống nhiều người

### Bước 1 — Chụp ảnh mỗi người

```bash
face_capture alice 5
```

- Mở cửa sổ preview với overlay bbox màu xanh khi detect OK
- Auto-chụp 5 frames cách nhau 500ms khi phát hiện đúng 1 khuôn mặt to (>= 100px)
- Bấm SPACE để chụp thủ công, Q/ESC để thoát

Options CLI (nếu không dùng `env.sh`):
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

### Bước 2 — Build database

```bash
face_enroll
```

Tương đương:
```bash
./enroll_faces \
    --dir faces --out db/faces_all.fdb \
    --det-model model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb \
    --recog-model model/face_recog/w600k_mbf_uint8_a733.nb \
    --recog-dim 512 --recog-bgr
```

- Duyệt mỗi subfolder → 1 identity
- Với mỗi ảnh: SCRFD detect largest face → align 112 → embedding
- Trung bình embeddings của cùng 1 người → 1 vector L2-norm/identity
- Save ra `.fdb` (~2 KB/identity)

DB size cho 1000 identities: ~2 MB.

### Bước 2b (tùy chọn) — Thêm 1 người không rebuild

```bash
# Từ 1 ảnh
face_add john /path/to/john.jpg

# Từ nhiều ảnh (chất lượng tốt hơn)
face_add jane jane1.jpg jane2.jpg jane3.jpg

# Nếu tên đã có:
face_add alice new.jpg --replace   # xóa entry cũ, thêm mới
face_add alice new.jpg --merge     # trung bình với entry cũ
```

Latency: ~250ms lần đầu (init NPU), ~20-30ms mỗi ảnh sau đó.

### Bước 3 — Chạy realtime match

USB cam:
```bash
face_run
```

RTSP IP camera:
```bash
face_run_rtsp 'rtsp://admin:pass@192.168.1.100:554/stream1'
```

Trên khung mặt hiển thị `<name> <score>` (ví dụ `alice 0.78`). Score dưới `--match-thr` → hiển thị `unknown`.

Options:
```
detect.nb                 Đường dẫn SCRFD .nb (positional, default nếu bỏ qua)
cam_id                    /dev/videoN (positional, default: 0)
--frames N                Chạy N frames rồi in bench summary và thoát
--recog-model PATH        Bật recognition, path .nb
--recog-dim N             Embedding dim (default: 512)
--recog-bgr               Feed BGR trực tiếp
--face-db PATH            Load .fdb
--match-thr F             Cosine similarity threshold (default: 0.35)
--source URL              RTSP/HTTP/file source (GStreamer). Overrides cam_id.
--gst-pipeline STR        Custom GStreamer pipeline (must end with `! appsink`)
--gst-latency MS          RTSP jitter buffer latency (default 100ms)
--windowed / --fullscreen (default: fullscreen)
```

Không có `--recog-model` → chạy như detect-only.

## Tuning `--match-thr`

| Ngưỡng | Behavior |
|---|---|
| 0.20 | Cực kỳ dễ dãi — dễ nhầm người |
| 0.35 (default) | An toàn cân bằng, self-match 0.75+ luôn pass |
| 0.45 | Chặt — chỉ match khi rất giống (giảm false positive) |
| 0.55 | Rất chặt — có thể miss same-person do lighting |

Tinh chỉnh cho hệ thống thực:
1. Chụp 5-10 ảnh khác nhau (góc, ánh sáng) của cùng 1 người
2. Chạy `face_run` với `--match-thr 0.10` — quan sát score self-match
3. Chọn `--match-thr = (min self-score) - 0.05`
4. Test với người lạ — score nên < 0.30

## Model info

### SCRFD 2.5g_bnkps 640
- Nguồn: InsightFace [SCRFD](https://github.com/deepinsight/insightface/tree/master/detection/scrfd)
- Input: 640×640 BGR uint8 (letterbox từ frame gốc)
- Output: 9 tensors — 3 FPN levels (strides 8/16/32) × [score, bbox, kps]
- Số anchor: 16800 (feature-grid, 2 anchor/location)
- Bbox format: **distance form** `(l, t, r, b)` từ anchor center
- Score: sigmoid baked-in

### MobileFaceNet (w600k_mbf)
- Nguồn: InsightFace buffalo_s bundle
- Input: 112×112 (align từ 5 landmarks ArcFace canonical)
- Preprocessing baked-in: mean=127.5, scale=1/127.5, reverse_channel=true → dùng flag `--recog-bgr`
- Output: 512-D embedding, L2-normalized

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

- **MJPG capture bottleneck**: USB MJPG capture ~15-19ms. Có thể tách capture thread (đã làm).
- **NPU chỉ có 1 core**: detect + recognize sharing → không parallel được. Recognition đã rất nhanh (~3ms) nên không phải bottleneck.
- **Không có drift/liveness**: chỉ dùng embedding cosine. Không phòng chống spoofing bằng ảnh in.
- **RetinaFace đã bỏ**: các phiên bản trước dùng RetinaFace ResNet50; hiện đã chuyển hoàn toàn sang SCRFD 2.5g (nhẹ hơn 20×, nhanh hơn 5-10× trên NPU, chất lượng recognition tương đương). Snapshot code cũ ở `legacy/`.
