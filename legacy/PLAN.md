# Plan: Inference RetinaFace với input Camera trên Orange Pi A733

**Model**: `model/Retinaface_resnet50_320_uint8_a733.nb`
**Target**: Realtime face detection từ USB camera → NPU A733 → hiển thị bbox + 5 landmarks.

---

## 0. Tổng quan môi trường (đã xác thực)

| Thành phần | Chi tiết |
|---|---|
| NPU | VIPLite driver 2.0.3.2 (Allwinner A733) |
| Header NPU | `/usr/include/vip_lite.h`, `/usr/include/vip_lite_common.h` |
| Lib NPU | `/lib/libNBGlinker.so`, `/lib/libVIPhal.so` |
| Model input | `1 × 3 × 320 × 320 uint8` (NHWC, none-quant, data_format=2) |
| Model output 0 | `4 × 4200` — bbox regression |
| Model output 1 | `2 × 4200` — classification (bg / face) |
| Model output 2 | `10 × 4200` — 5 landmarks × (x, y) |
| Camera | `/dev/video0` — Rapoo USB, MJPG, hỗ trợ 320×240 / 640×480 / 1280×720 @30fps |
| OpenCV | 4.6 (arm64, có `pkg-config opencv4`) |
| SDK tham khảo | `/home/orangepi/ai-sdk/` (đặc biệt `examples/yolov5/`, `examples/libawnn_viplite/`) |
| Inference NPU đo được | ~17 ms/frame (từ log demo) |

**Test binary có sẵn** `retinaface_demo_a733` đã chạy tốt với `model/test.jpg`:
- Detect 1 face @ `[244, 46, 363, 209]`, confidence 100%
- 5 landmarks output đầy đủ

---

## Giai đoạn 1 — Xác thực stack (nhanh, không viết code)

- [ ] **1.1** Chụp 1 frame từ camera bằng `ffmpeg`:
  ```bash
  ffmpeg -f v4l2 -input_format mjpeg -video_size 640x480 -i /dev/video0 \
         -frames:v 1 -y frame.jpg
  ```
- [ ] **1.2** Chạy binary có sẵn với frame vừa chụp:
  ```bash
  ./retinaface_demo_a733 -nb model/Retinaface_resnet50_320_uint8_a733.nb \
                         -i frame.jpg -l 1 -m 20
  ```
- [ ] **1.3** Kiểm tra output có phát hiện đúng khuôn mặt không.
- [ ] **1.4** So sánh output ảnh (`out_retinaface.png`) với ảnh gốc → xác nhận chuỗi preprocess (letterbox, color order, normalization) khớp với model.

**Kết quả mong đợi**: model detect đúng trên ảnh camera thật → xanh đèn qua Giai đoạn 2.

---

## Giai đoạn 2 — Chọn kiến trúc pipeline

### Hướng A — Script wrapper (nhanh, ít code, ~5 FPS)
- Loop shell/python: grab frame (`ffmpeg` hoặc `v4l2-ctl`) → gọi binary → parse stdout → overlay.
- **Ưu**: không cần build gì mới.
- **Nhược**: fork process mỗi frame, không tái sử dụng NPU context, chậm.

### Hướng B — App C++ real-time (khuyến nghị, ~25–30 FPS)
- 1 process, init NPU 1 lần, loop capture + inference + render.
- Dùng lại wrapper `libawnn_viplite` trong `/home/orangepi/ai-sdk/examples/`.
- Cấu trúc mô phỏng theo `ai-sdk/examples/yolov5/`.

**Quyết định**: [ ] Hướng A / [ ] Hướng B

---

## Giai đoạn 3 — Xây dựng module (theo Hướng B)

### Cấu trúc thư mục đề xuất
```
retinaface_camera/
├── Makefile              # copy từ ai-sdk/examples/yolov5/Makefile, chỉnh flags
├── main.cpp              # main loop: capture → infer → draw → display
├── retinaface_pre.cpp    # preprocess: letterbox 320×320, HWC uint8
├── retinaface_pre.h
├── retinaface_post.cpp   # decode anchors + NMS + landmarks
├── retinaface_post.h
├── anchors.cpp           # generate 4200 prior boxes
├── anchors.h
└── model/                # symlink tới file .nb
```

### 3.1 Camera capture
- [ ] Mở camera bằng OpenCV:
  ```cpp
  cv::VideoCapture cap(0, cv::CAP_V4L2);
  cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
  cap.set(cv::CAP_PROP_FRAME_WIDTH,  640);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
  cap.set(cv::CAP_PROP_FPS, 30);
  ```

### 3.2 Preprocess
- [ ] Letterbox resize BGR 640×480 → 320×320 (giữ tỉ lệ, pad 0).
- [ ] Xác nhận có cần trừ mean `(104, 117, 123)` hay để raw uint8 (Rủi ro #1 phía dưới).
- [ ] Copy vào input buffer NPU (`vip_bind_input`).

### 3.3 NPU inference
- [ ] Copy `ai-sdk/examples/libawnn_viplite/` sang project.
- [ ] Init 1 lần: `awnn_init()` → `awnn_create(model_path)`.
- [ ] Per-frame: `awnn_run()` → lấy 3 output pointer.
- [ ] Cleanup: `awnn_destroy()` khi thoát.

### 3.4 Postprocess
- [ ] Generate priors (4200 anchors):
  - Feature maps: `40×40`, `20×20`, `10×10`
  - `min_sizes = [[16, 32], [64, 128], [256, 512]]`
  - `steps = [8, 16, 32]`
  - `variances = [0.1, 0.2]`
- [ ] Decode bbox: `bbox = prior + loc * variance`.
- [ ] Decode 5 landmarks tương tự.
- [ ] Softmax cls → filter `score > 0.5`.
- [ ] NMS với IoU threshold = 0.4, top-K = 100.
- [ ] Rescale bbox/landmarks từ letterbox 320 → tọa độ frame gốc 640×480.

### 3.5 Draw + display
- [ ] `cv::rectangle` bbox, `cv::circle` 5 landmarks, `cv::putText` score.
- [ ] Overlay FPS + latency breakdown (capture/pre/npu/post/draw).
- [ ] `cv::imshow` nếu có desktop, hoặc encode `cv::VideoWriter` MJPEG stream nếu headless.

---

## Giai đoạn 4 — Build & benchmark

- [ ] Compile:
  ```bash
  g++ -O2 -std=c++17 *.cpp $(pkg-config --cflags --libs opencv4) \
      -lVIPhal -lNBGlinker -lpthread -lrt -o retinaface_camera
  ```
- [ ] Chạy: `./retinaface_camera`
- [ ] Đo FPS + latency từng stage.
- [ ] Target: **~25–30 FPS end-to-end** với input camera 640×480.

---

## Giai đoạn 5 — Tối ưu (tùy chọn)

- [ ] **Multi-threading**: tách 3 thread (capture / inference / render) + queue có bound.
- [ ] **Zero-copy YUYV**: convert YUV→RGB trên GPU/OpenCL thay vì CPU.
- [ ] **Double-buffered NPU**: chạy song song 2 inference (khảo sát `ai-sdk/examples/multi_thread/`).
- [ ] **Encode output**: RTSP / MJPEG-HTTP stream để xem từ máy khác.
- [ ] **Face tracker**: thêm SORT / ByteTrack để giảm số frame chạy detection.

---

## Rủi ro cần verify sớm

| # | Rủi ro | Cách verify |
|---|---|---|
| 1 | Normalization — trừ mean (104,117,123) hay raw uint8? | Chạy demo với `test.jpg` (đã pass ⇒ nghi là raw uint8) → so kết quả với ảnh crop. |
| 2 | Layout tensor: NHWC vs NCHW | Log demo cho `dim 3 320 320 1` ⇒ NHWC. Preprocess byte order phải là H·W·C. |
| 3 | Color channel: BGR hay RGB? | Test cả 2 chiều trên cùng ảnh → chọn chiều cho bbox chuẩn. RetinaFace gốc dùng BGR. |
| 4 | Anchor config khớp model? | Nếu bbox lệch → thử tổ hợp `min_sizes / steps` khác. Có thể dump NBG bằng `ai-sdk/tools/nbinfo`. |
| 5 | Có `DISPLAY` không? | `echo $DISPLAY` — nếu rỗng thì làm MJPEG stream / ghi file. |
| 6 | NPU memory (`-m`) | Demo dùng `20 MB`. Nếu OOM tăng lên 40–60 MB. |

---

## Checklist tổng

- [ ] G1. Verify stack với ảnh camera thật.
- [ ] G2. Chọn Hướng A hoặc B.
- [ ] G3. Implement pipeline (5 module).
- [ ] G4. Build + benchmark FPS.
- [ ] G5. Tối ưu (nếu cần).

---

## Ghi chú thay đổi

| Ngày | Ghi chú |
|---|---|
| 2026-08-20 | Khởi tạo plan. |
