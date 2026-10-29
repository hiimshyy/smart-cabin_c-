# Bàn giao tạm thời — Cabin Occupancy

Cập nhật: 2026-10-27. Tài liệu này là điểm bắt đầu khi tiếp tục tính năng occupancy.

## Trạng thái vận hành hiện tại

- Thiết bị được chuẩn hóa cho cabin/thang máy `2`.
- Camera PostgreSQL mặc định: `#6` (slave 2, đã xác nhận connected tại thời điểm cấu hình).
- `face_cabin` chạy tay mặc định tắt Socket.IO/floor-call để test occupancy: `FACE_CABIN_SOCKETIO=0`.
- Service production override `FACE_CABIN_SOCKETIO=1`, nhận `FACE_IOT_WS` từ `/etc/face-recog-app.env` và bật door-gated adaptive inference.
- Service dùng `SIGTERM`, chạy headless và đã được cài tại `/etc/systemd/system/face-recog-app.service`.
- Trạng thái runtime có thể thay đổi; kiểm tra bằng `systemctl is-active face-recog-app.service` và `pgrep -a face_recog_app` trước khi test tay. Không chạy service và app thủ công đồng thời.
- Unit vẫn được enable từ trước, vì vậy có thể tự chạy sau reboot nếu không disable riêng.
- SQLite `db/residents.db` có row cabin 2 để audit/FK dùng đúng `cabin_id=2`. Backup trước khi thêm row: `/tmp/residents.db.before-cabin2-20261027142511.db`.

## Dữ liệu/config cần giữ

- Config gốc đã hiệu chỉnh: `config/elve_02_occupancy.json`.
- Mapping runtime: `config/occupancy_cam2.json -> elve_02_occupancy.json`.
- State production: `db/occupancy_state_cam2.json`.
- State cabin 1 cũ còn được giữ tại `db/occupancy_state_cam1.json`; không dùng cho cabin 2.
- Video regression chính:
  - `recordings/cabin_1_20261008_180247_277_32.avi`.
  - `recordings/cabin_2_20261027_161633_113_1.avi`.

Zone cabin 2 hiện dùng một polygon chuẩn hóa trên frame tham chiếu 1920×1080:

```json
{
  "inside_direction_degrees": 68,
  "min_crossing_displacement": 0.05,
  "polygon": [
    { "x": 0.469727, "y": 0.602431 },
    { "x": 0.290039, "y": 0.725694 },
    { "x": 0.324219, "y": 0.935764 },
    { "x": 0.542969, "y": 0.770833 }
  ]
}
```

Người dùng đã xác nhận polygon hiển thị đạt yêu cầu.

## Những phần đã hoàn thành

- Config JSON v1, validation đúng một zone và chuyển normalized → pixel.
- State persistence atomic, worker bounded/throttled, shutdown flush.
- Entry/Exit event-based bằng bottom-center bbox.
- Confidence `Certain/Uncertain`, RTSP outage, prediction-only, track loss và aborted crossing.
- Ghost ID churn, one-to-many ambiguity và chống duplicate count.
- Occupancy slot theo track: track không có Entry/slot không thể phát Exit hoặc trừ count của người khác.
- Segment-crossing cho trường hợp live latest-frame slot bỏ frame nằm trong polygon.
- ByteTrack giữ first observation của tentative track tối đa 1 giây để hỗ trợ người đi nhanh mà vẫn giữ `confirm_hits=3`.
- Candidate được giữ khi điểm chân rời polygon qua sticky projection band; đây là root cause thật của fast-entry video.
- CLI, `env.sh`, main loop, HUD/overlay, system metrics và persistence wiring.
- Cabin 2, camera `#6`, config/state suffix 2; manual-test profile tắt Socket.IO còn systemd production profile bật Socket.IO/adaptive inference.
- Debug trace mức `debug` chỉ log numeric track ID và side transition (`occupancy-track`), không log tọa độ hoặc PII.

## Bằng chứng kiểm thử

### Unit/integration

Sau sửa cuối:

- `test_occupancy_counter`: 44 checks, 0 failed.
- `test_byte_tracker`: 11 checks, 0 failed.
- `bash tests/run_tests.sh`: `ALL TESTS PASSED`.
- `make -j4`: PASS; binary `face_recog_app` đã được build lại.
- `clang-format --dry-run --Werror`: PASS trên file C++ đã đổi.
- Diagnostics và `git diff --check`: sạch.

### Replay video thật qua NPU

`recordings/cabin_1_20261008_180247_277_32.avi`:

```text
ENTRY track=1 count=1
EXIT  track=1 count=0
```

`recordings/cabin_2_20261027_161633_113_1.avi` trước sửa sticky band: không có event dù track đi qua zone.

Cùng video sau sửa cuối, replay đủ 1.347 frame ở khoảng 15 FPS:

```text
ENTRY track=1 count=1
EXIT  track=1 count=0
ENTRY track=2 count=1
EXIT  track=2 count=0
```

Kết quả đúng nội dung video: fast entry → exit → quay lại cabin → exit cuối, tức `0 → 1 → 0 → 1 → 0`.

GStreamer có warning query-position và reconnect khi AVI tới EOF; đây là hành vi replay file, không phải lỗi crossing. EOF/prediction có thể làm state cuối thành `Uncertain` dù event cuối tại thời điểm crossing là `Certain`.

## Root cause đã tìm được từ trace

Trace của video fast-entry:

```text
outside → polygon → sticky band ngoài polygon → inside
```

Code cũ ép observation sticky về `outside`, hủy `entry_candidate`, nên không còn candidate khi track sang `inside`. Bản sửa chỉ fallback về outside khi không có entry/exit candidate; candidate hợp lệ được giữ xuyên sticky band.

Hạ person threshold từ 0.50 xuống 0.35 đã được thử một phần nhưng không tạo Entry trong phần đầu và run bị người dùng dừng giữa chừng. Không áp dụng thay đổi threshold production; mặc định vẫn là high 0.50, low 0.10.

## Phần còn thiếu và rủi ro

1. **Chưa live-retest sau bản sửa sticky-band cuối.** Live trước đó đã tái hiện lỗi và một lượt chậm từng đếm đúng, nhưng kết luận cuối hiện dựa trên replay NPU của chính recording lỗi.
2. **Chưa stress test nhiều người:** hai người sát nhau, đi ngược chiều đồng thời, che khuất, nhập/tách bbox và đi liên tiếp nhanh.
3. **Chưa chạy soak test service dài hạn** để theo dõi drift, state persistence qua restart và độ ổn định RTSP.
4. **Nhiệt độ live từng đạt 86.4°C** khi chạy liên tục không door-gating; cần bảo đảm tản nhiệt trước soak test.
5. **Confidence có thể thành `Uncertain` sau prediction-only/track loss/EOF.** Đây là contract hiện tại; cần đánh giá UX/vận hành thực tế xem có cần cơ chế reconcile bên ngoài hay không.
6. API `reconcile(count)`/`reconcile_empty()` đã có trong code nhưng chưa có command/UI/operator endpoint để gọi khi cần hiệu chỉnh count thực tế.
7. UI chỉ vẽ overlay; chưa có editor click-to-save zone. Zone hiện được chỉnh bằng JSON.
8. V1 chỉ hỗ trợ đúng một counting zone.
9. Chưa tạo commit. Toàn bộ feature và các thay đổi trước đó vẫn nằm trong working tree của `main`; phải review phạm vi diff trước khi commit.
10. Config, state và recordings là dữ liệu deployment/runtime. Không đưa state/recordings vào commit; quyết định riêng xem zone JSON cabin 2 có được version-control hay triển khai ngoài repo.

## Cách tiếp tục lần sau

### Kiểm tra repo và runtime

```bash
cd /home/orangepi/face_recog_app
git status --short
systemctl is-active face-recog-app.service
pgrep -a face_recog_app
```

### Chạy live bằng MobaXterm

Mở SSH session mới có X11 forwarding. Trước khi source, `echo "$DISPLAY"` phải có dạng `localhost:10.0`.

```bash
cd /home/orangepi/face_recog_app
unset FACE_CABIN_ID FACE_CAMERA_SELECT FACE_CABIN_SOCKETIO
unset FACE_IOT_WS FACE_EVENT_WS FACE_RECORD_DIR FACE_CABIN_NO_PREVIEW
source ./env.sh
export FACE_CABIN_NO_PREVIEW=0
export FACE_CABIN_LOG_LEVEL=info
sudo systemctl stop face-recog-app.service
face_cabin
```

Giá trị mong đợi:

```text
FACE_CABIN_ID=2
FACE_CAMERA_SELECT=#6
FACE_CABIN_SOCKETIO=0
DISPLAY=localhost:10.0  # số có thể khác
```

### Live acceptance còn thiếu

- Bắt đầu khi biết chắc cabin trống; nếu cần reset/reconcile state, chỉ làm sau khi operator xác nhận cabin vật lý trống.
- Một người vào nhanh: `ENTRY`, count `0 → 1`.
- Cùng người ra nhanh: `EXIT`, count `1 → 0`.
- Lưỡng lự/quay lại cùng phía: không tạo event giả.
- Hai chu kỳ liên tiếp không drift.
- Sau đó mới thử nhiều người/che khuất.

Bật `FACE_CABIN_LOG_LEVEL=debug` chỉ khi cần xem `occupancy-track`; mức debug sinh nhiều log mỗi frame. Trở lại `info` cho vận hành bình thường.
