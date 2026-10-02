# Design — ByteTrack-lite Person Tracker

**Spec ID**: `tracker-upgrade`

## 1. Quyết định kiến trúc

Thay Greedy IoU bằng một backend ByteTrack-lite nội bộ:

```text
video frame + monotonic timestamp
            │
            ├── tracker.predict(dt) ───────────────┐
            │                                      │ predicted boxes
            └── YOLO (theo person_every)           │
                    │ decode >= low threshold      │
                    ├── high detections ───────────┤
                    └── low detections ────────────┤
                                                   ▼
                                      Hungarian two-stage association
                                                   │
                                      lifecycle + duplicate resolution
                                                   │ TrackView snapshot
                    SCRFD faces ───────────────────┤
                                                   ▼
                                      one-to-one face association
                                                   │
                                      recognition/cache/interaction
```

Lý do chọn:

- Kalman + Hungarian giải hai thiếu hụt lớn nhất của code hiện tại: không predict và greedy local.
- ByteTrack tái sử dụng low-confidence boxes, không cần thêm model hoặc NPU context.
- Cabin có ít người nên assignment matrix nhỏ; inference tiếp tục là chi phí chính.
- Tracker vẫn online, một thread, phù hợp pipeline realtime hiện tại.

## 2. Thành phần và file

| File | Vai trò |
|---|---|
| `src/tracker_types.h` (mới) | `TrackState`, `TrackView`, `FrameContext`, identity fields |
| `src/person_tracker.h` (mới) | Interface chung cho greedy/byte backend |
| `src/tracker_greedy.{h,cpp}` (mới/đổi tên) | Adapter hành vi tracker hiện tại để rollback |
| `src/tracker.{h,cpp}` | ByteTrack-lite orchestration và public implementation |
| `src/linear_assignment.{h,cpp}` (mới) | Hungarian rectangular assignment |
| `src/kalman_box_tracker.{h,cpp}` (mới) | Predict/correct bbox state |
| `src/face_track_assoc.{h,cpp}` (mới) | One-to-one face/person association |
| `src/yolo_post.{h,cpp}` | Decode detections từ low threshold |
| `src/main.cpp` | Predict mỗi frame, update khi detector chạy, dùng TrackView/IDs |
| `src/overlay.cpp` | Vẽ TrackView, phân biệt predicted/lost nếu cần |
| `src/resident_reconcile.*` | Invalidate identity trực tiếp qua tracker API |
| `src/app_config.*` | Parse/validate tracker mode, low threshold, timeout |
| `src/benchmark.*` | Đo predict/associate nếu bật benchmark |
| `tests/test_linear_assignment.cpp` | Solver tests |
| `tests/test_kalman_box_tracker.cpp` | Motion tests |
| `tests/test_tracker.cpp` | ByteTrack lifecycle/identity/hot reload |
| `tests/test_face_track_assoc.cpp` | Face association tests |
| `tests/test_tracker_replay.cpp` | Deterministic sequence replay/A-B |
| `tests/fixtures/tracker/*.json` | Không ảnh, không PII; detection sequences |

`tracker_types.h` tách data-only types để overlay, reconciliation và tests không phụ thuộc concrete
backend. Nếu việc đổi tên `tracker.cpp` tạo diff quá lớn, có thể giữ greedy trong cùng module ở commit
adapter đầu tiên rồi tách sau; public API cuối cùng không thay đổi.

## 3. Public API

```cpp
enum class TrackerMode : uint8_t { Greedy, Byte };
enum class TrackState : uint8_t { Tentative, Tracked, Lost, Removed };

struct FrameContext {
    int frame_id = 0;
    double timestamp_ms = 0.0;  // steady/monotonic
    int frame_width = 0;
    int frame_height = 0;
};

struct TrackIdentity {
    int64_t resident_id = -1;
    std::string display_name;
    float similarity = -1.0f;
    double last_recognition_ms = -1.0;
};

struct TrackView {
    int id = -1;
    TrackState state = TrackState::Tentative;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float detection_score = 0;
    bool predicted_only = false;
    TrackIdentity identity;
};

struct TrackerConfig {
    float high_threshold = 0.50f;
    float low_threshold = 0.10f;
    float primary_iou_gate = 0.30f;
    float secondary_iou_gate = 0.20f;
    int confirm_hits = 3;
    double lost_timeout_ms = 1200.0;
    double ghost_timeout_ms = 30000.0;
    double max_predict_gap_ms = 2000.0;
    double known_retry_ms = 3000.0;   // migration from ~90 frames @30 fps
    double unknown_retry_ms = 1000.0; // migration from ~30 frames @30 fps
};

class PersonTracker {
public:
    virtual ~PersonTracker() = default;
    virtual void predict(const FrameContext& frame) = 0;
    virtual void update(const std::vector<PersonDet>& detections,
                        const FrameContext& frame) = 0;
    virtual std::vector<TrackView> active_tracks() const = 0;
    virtual std::optional<TrackView> find_track(int track_id) const = 0;
    virtual bool needs_recognition(int track_id, double now_ms) const = 0;
    virtual void record_recognition(int track_id, int64_t resident_id,
                                    std::string display_name, float similarity,
                                    double now_ms) = 0;
    virtual ReconcileResult invalidate_catalog(
        const CatalogDiff& diff, bool invalidate_unknown) = 0;
};
```

Không trả `Track*`. Main thu `TrackView` snapshot sau update và face association dùng IDs. Mutation
recognition thực hiện qua method, rồi main có thể lấy lại view khi cần overlay.

## 4. Kalman box model

### 4.1 State và measurement

```text
x = [cx, cy, w, h, vx, vy, vw, vh]^T
z = [cx, cy, w, h]^T
```

Transition với `dt`:

```text
cx' = cx + vx*dt
cy' = cy + vy*dt
w'  = w  + vw*dt
h'  = h  + vh*dt
velocity giữ nguyên
```

Measurement matrix chọn bốn position/size components. `dt` đổi milliseconds sang seconds và clamp
trong khoảng an toàn. Process noise tăng theo `dt`; measurement noise giữ config compile-in ban đầu,
sau đó tune từ replay. Dùng `cv::KalmanFilter(8,4)` để giảm rủi ro tự viết matrix math; tests tracker
phải link OpenCV.

### 4.2 Initialization

Track mới:

- Position/size lấy từ detection.
- Velocity bằng 0.
- Position covariance vừa phải; velocity covariance lớn hơn để học chuyển động nhanh.
- `last_predict_ms = frame.timestamp_ms`.

### 4.3 Invalid time và reconnect

Nếu `dt <= 0`, dùng `dt=0` và không extrapolate. Nếu `dt > max_predict_gap_ms`:

- Zero velocity.
- Tăng uncertainty hoặc reinitialize motion từ last measurement.
- Không extrapolate xuyên toàn bộ outage.
- Geometry track vẫn theo lifecycle timeout; removed track chỉ có thể quay lại qua ghost identity.

### 4.4 Box sanitation

- `w/h >= 1` pixel.
- Chuyển `(cx,cy,w,h)` thành `(x1,y1,x2,y2)` với finite checks.
- Output box clamp vào frame cho overlay; internal center có thể giữ margin nhỏ để người đi ra/vào.
- Detection có NaN/Inf hoặc box area không dương bị loại trước assignment.

## 5. Hungarian solver

API đề xuất:

```cpp
struct Assignment {
    std::vector<std::pair<int,int>> matches;
    std::vector<int> unmatched_rows;
    std::vector<int> unmatched_cols;
};

Assignment solve_linear_assignment(
    const std::vector<std::vector<float>>& cost,
    float forbidden_cost);
```

Contract:

- Rectangular matrix; transpose nội bộ nếu cần.
- Empty rows/columns trả unmatched phù hợp.
- Cost không finite được xem forbidden.
- Sau solver, caller vẫn kiểm tra gate để chống solver chọn forbidden padded cells.
- Tie-break bằng row index rồi column index.
- Không dùng random.

Với tối đa vài người, O(n³) đủ. Có thể thêm hard cap detections sau NMS để bảo vệ input lỗi, nhưng
không cap thấp đến mức mất người thật.

## 6. Association cost và gating

Baseline cost:

```text
cost = 1 - IoU(predicted_track_box, detection_box)
```

Primary candidate hợp lệ nếu `IoU >= primary_iou_gate`. Secondary low-confidence pass có thể dùng
gate thấp hơn. Nếu replay chứng minh IoU đơn thuần vẫn miss khi chuyển động nhanh, mở rộng thành:

```text
cost = wiou * (1 - IoU) + wcenter * normalized_center_distance
```

với hard gate: cặp chỉ hợp lệ nếu IoU đủ hoặc center distance trong giới hạn. Không đưa composite
cost vào bản đầu nếu chưa có data chứng minh, để giữ thuật toán dễ kiểm tra.

## 7. ByteTrack update sequence

```text
INPUT: all valid detections >= low threshold, predicted tracks

1. split detections:
     HIGH score >= high_threshold
     LOW  low_threshold <= score < high_threshold

2. primary pool:
     confirmed Tracked + Lost còn timeout
   Hungarian(primary pool, HIGH)
   matched -> Kalman correct, state=Tracked

3. secondary pool:
     unmatched tracks từng ở Tracked (không ưu tiên stale Lost)
   Hungarian(secondary pool, LOW)
   matched -> Kalman correct, state=Tracked

4. tentative association:
     existing Tentative với remaining HIGH
   match -> hits++; confirm nếu đủ
   miss -> Removed

5. create:
     remaining HIGH -> Tentative tracks
     remaining LOW  -> discard

6. lifecycle:
     unmatched confirmed -> Lost
     Lost quá timeout -> Removed + optional ghost identity

7. duplicate resolution:
     nếu two live tracks overlap rất cao và cùng resident_id/trajectory,
     giữ track lâu hơn/nhiều hits hơn; không merge chỉ do một frame overlap
```

Thứ tự pool và exact matching threshold phải được khóa bằng tests. Lost tracks được primary match
với high detection để reactivation; low detection chỉ cứu track vừa miss nhằm tránh resurrect stale
track từ noise.

## 8. YOLO decoder migration

Hiện `decode(... score_thresh ...)` vừa early-filter objectness vừa filter final score. Đổi tên tham số
thành `decode_threshold` và main truyền `person_low_thr`. Output vẫn NMS theo score giảm dần, nên high
detection thắng low duplicate.

Validation:

```text
0 <= person_low_thr < person_high_thr <= 1
```

`--person-thr` map vào high threshold để giữ CLI. Thêm `--person-low-thr`. Không tạo hai lần decode
NPU output; chỉ decode một lần rồi partition để giữ latency thấp.

## 9. Lifecycle và public visibility

- Tentative không xuất hiện trong interaction/recognition; overlay DEBUG có thể vẽ nếu cần.
- Tracked xuất hiện trong active views.
- Lost ngắn hạn có thể vẽ predicted box nhưng không được tạo recognition mới khi không có face.
- Removed không xuất hiện.
- `active_tracks()` trả Tracked và tùy policy Lost chưa hết timeout; trường `predicted_only` cho overlay.
- Interaction input chỉ hình thành từ face đã association với eligible Tracked track, giữ semantics hiện tại.

Timeout theo milliseconds. `track_max_miss` được giữ trong rollout cho greedy; byte dùng
`track_lost_ms`. Nếu cần backward migration, khi chỉ có `track_max_miss`, derive một lần từ nominal
30 FPS và log rõ nguồn; cấu hình mới ưu tiên milliseconds.

## 10. Face-to-track association

### 10.1 Candidate gate

Với mỗi TrackView Tracked:

```text
head_roi = top head_ratio (default 0.5) của person box
face center phải nằm trong head_roi có margin nhỏ
face width/person width và face height/person height phải trong range hợp lý
```

### 10.2 Cost

```text
dx = |face_cx - expected_head_cx| / person_width
dy = |face_cy - expected_head_cy| / head_roi_height
size_penalty = deviation của face/person ratio
cost = wx*dx + wy*dy + ws*size_penalty
```

Hungarian giải faces × tracks. Forbidden gate ngăn assignment vô lý. Nếu face đã map cùng track ở
frame trước và geometry vẫn hợp lệ, history bonus nhỏ dùng làm tie-break; bonus không được thắng hard
gate.

API trả vector track IDs theo face index (`-1` = orphan), tránh pointer lifetime.

## 11. Recognition identity và ghost

### 11.1 Single source of truth

Identity được đặt trong track:

```cpp
int64_t resident_id = -1;
std::string display_name; // UI only
float match_similarity;
double last_recognition_ms;
```

Loại bỏ dần `track_resident_id` map ngoài `main.cpp`. Trong commit chuyển tiếp có thể giữ adapter,
nhưng trạng thái cuối chỉ có một authoritative resident mapping.

### 11.2 Ghost

```cpp
struct GhostIdentity {
    int old_track_id;
    int64_t resident_id;
    double first_seen_ms;
    double removed_ms;
    Box last_box;
};
```

Khi Removed track có known resident, tạo ghost. Khi track mới recognition thành cùng resident:

1. Ghost còn TTL.
2. Không active track khác sở hữu resident.
3. Recognition đủ threshold/confirmation.
4. Nếu có nhiều ghost, chọn recent nhất và có geometry hợp lý nhất.
5. Kế thừa old track ID/first seen; xóa ghost.

Display name không tham gia equality. Hot reload changed/removed resident purge ghost tương ứng.

### 11.3 Identity conflict

Nếu track cache resident A nhưng recognition frame mới ra B:

- Không đổi ngay từ một frame.
- Lưu pending resident + streak/similarity evidence.
- Chỉ chuyển khi đủ confirm policy hoặc A đã invalidated bởi catalog reload.
- Log numeric IDs ở DEBUG/WARN rate-limited, không log labels.

## 12. Resident hot reload integration

`ResidentCatalog` diff gọi:

```cpp
tracker.invalidate_catalog(diff, invalidate_unknown);
```

Behavior:

- `added`: clear recognition schedule của unknown tracks.
- `changed`: clear identity/cache của tracks/ghosts mang resident đó.
- `removed`: clear identity/cache và ownership; interaction affected sessions reconcile.
- Geometry/Kalman/lifecycle giữ nguyên.

`resident_reconcile` nhận list affected track IDs từ tracker để drop mapping/session cần thiết. Sau khi
Track trực tiếp giữ resident ID, module không cần tự duy trì map song song.

## 13. Main loop integration

Pseudocode:

```cpp
FrameContext fc{frame_id, steady_now_ms, frame.cols, frame.rows};

tracker->predict(fc);                 // every received frame

if (frame_id % person_every == 0) {
    yolo.decode(outputs, pre, person_low_thr, nms_thr, persons);
    tracker->update(persons, fc);     // split high/low internally
}

auto track_views = tracker->active_tracks();
auto face_track_ids = associate_faces(faces, track_views, previous_links);

for each face:
    track_id = face_track_ids[i];
    if track_id >= 0 && tracker->needs_recognition(track_id, fc.timestamp_ms):
        run recognition
        tracker->record_recognition(track_id, resident_id, display_name, sim, now)

build interaction subjects from stable track IDs
draw TrackView snapshot
```

Nếu không có tracker, SCRFD-only branch giữ nguyên.

Lưu ý: predict phải chạy sau khi có frame dimensions. Khi frame wait timeout chỉ để poll DB và không
có video frame, không advance motion bằng fake frame; timestamp gap được xử lý lúc frame kế tiếp.

## 14. Configuration

### Giữ lại

- `--person-thr`: high threshold.
- `--person-every`.
- `--track-iou`: primary IoU gate.
- `--recog-retry`: greedy compatibility; byte chuyển sang ms nội bộ hoặc thêm flag sau.
- `--track-max-miss`: greedy compatibility.

### Thêm

```text
--tracker-mode greedy|byte
--person-low-thr F
--track-lost-ms N
```

Secondary gate, confirm hits, max predict gap có thể giữ compile-in ở version đầu để tránh quá nhiều
knobs. Chỉ expose khi replay/live data chứng minh cần field tuning.

Parser phải validate và trả exit code config hiện hành; không silently clamp threshold đảo ngược.

## 15. Logging và metrics

Startup:

```text
[track] mode=byte high=0.50 low=0.10 iou=0.30 lost_ms=1200 confirm=3
```

INFO/WARN:

- Rate-limited tracker health: active/tracked/lost counts.
- Invalid timestamp/reconnect motion reset.
- Không log mỗi match.

DEBUG/TRACE:

- `track_id`, detection tier, IoU/cost, state transition.
- `resident_id` chỉ khi cần chẩn đoán identity; không labels.

Benchmark:

- `track_predict_ms`.
- `track_associate_ms`.
- active/lost count.
- IDSW chỉ có trong replay có ground truth, không tự suy đoán production.

## 16. Test design

### 16.1 Hungarian

- Known optimal 2×2, 3×3.
- Rectangular 2×4 và 4×2.
- Empty dimensions.
- Forbidden rows/columns.
- Equal costs deterministic.

### 16.2 Kalman

- Stationary measurements converge.
- Constant horizontal/vertical motion.
- 1–N missing observations predict forward.
- Variable dt gives comparable physical motion.
- Negative/NaN/large gap safe reset.
- Width/height stay positive.

### 16.3 Tracker replay fixtures

Fixture schema đề xuất:

```json
{
  "name": "two_people_crossing",
  "config": {"high": 0.5, "low": 0.1, "lost_ms": 1200},
  "frames": [
    {
      "t_ms": 0,
      "detections": [
        {"gt": 1, "x1": 10, "y1": 20, "x2": 90, "y2": 300, "score": 0.9}
      ]
    }
  ],
  "expect": {"max_id_switches": 0}
}
```

`gt` chỉ tồn tại trong test fixture/evaluator; production detection không có field này.

Fixtures:

- Single constant motion.
- Two crossing.
- Short full occlusion.
- Low-confidence bridge.
- Low-confidence false positive.
- Detector skipped frames.
- Variable FPS.
- Reconnect time gap.
- Duplicate overlapping detection.
- Entry/exit at frame boundary.

### 16.4 Face/identity

- Overlapping person boxes, two faces.
- Face orphan.
- One face eligible for two tracks.
- Same display name, different resident IDs.
- Same resident re-entry within TTL.
- Two active tracks claim same resident.
- Catalog add/change/remove.
- Logs contain no display label.

## 17. Performance plan

Benchmark cùng binary/config và cùng replay/video:

```text
greedy baseline:
  fps, e2e p50/p95, tracker p50/p95, IDSW, fragments, duplicate events

byte candidate:
  cùng metrics
```

Target:

- Tracker p95 <1 ms với ≤10 detections trên A733.
- E2E FPS regression ≤2%.
- IDSW/fragments giảm; mục tiêu ≥50% trên cabin evaluation set.
- Zero low-only track creation.
- Zero duplicate greeting do ID fragmentation trong acceptance clips.

HOTA/IDF1 dùng khi đủ ground truth; nếu annotation ít, report raw IDSW/fragments cùng scenario count.

## 18. Rollout

1. Merge backend interface + tests, default greedy.
2. Merge byte backend, A/B replay CI/local.
3. Orange Pi build và recorded-video run.
4. Cabin canary dùng `--tracker-mode byte`.
5. Soak, thu aggregate metrics không PII.
6. Đổi default byte khi acceptance đạt.
7. Giữ greedy rollback một release cycle.
8. Sau sign-off, tạo task riêng xóa legacy nếu muốn giảm maintenance.

Rollback không thay DB/data; chỉ đổi CLI mode và restart service.

## 19. Rủi ro và giảm thiểu

| Rủi ro | Giảm thiểu |
|---|---|
| Low threshold tăng false boxes | Low không tạo track; NMS + cap hợp lý |
| Kalman drift sau outage | max-gap reset, time-based lifecycle |
| Hungarian match geometry sai khi overlap | motion prediction, gates, deterministic replay |
| Face gắn sai person | one-to-one head-region cost |
| Hai người trùng tên bị merge | identity keyed bằng resident ID |
| Dual state tracker/map lệch nhau | chuyển authoritative resident mapping vào Track |
| Hot reload tạo duplicate interaction | reconcile tại frame boundary, regression tests |
| Performance regression | no ReID model, small matrices, benchmark gate |
| Rollout gây mất ổn định | backend flag, greedy fallback, canary |

## 20. References

- SORT: https://arxiv.org/abs/1602.00763
- ByteTrack: https://www.ecva.net/papers/eccv_2022/papers_ECCV/papers/136820001.pdf
- HOTA: https://arxiv.org/abs/2009.07736
- DeepSORT (ngoài scope, tham khảo tương lai): https://arxiv.org/abs/1703.07402

