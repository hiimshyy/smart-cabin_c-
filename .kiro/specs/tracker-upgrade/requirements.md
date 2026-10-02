# Requirements — Nâng cấp Person Tracker lên ByteTrack-lite

**Spec ID**: `tracker-upgrade`  
**Phụ thuộc**: pipeline YOLO/SCRFD hiện tại, `resident-db-layer`, `resident-hot-reload`,
`main-cpp-refactor`, `system-logging`  
**Tham khảo**: SORT (Kalman + Hungarian), ByteTrack (association hai mức confidence), HOTA

## 1. Bối cảnh và vấn đề

Pipeline vận hành hiện tại dùng YOLOv5s phát hiện người và một tracker tự viết trong
`src/tracker.{h,cpp}`. Tracker ghép detection với track bằng cách tính toàn bộ IoU, sắp xếp giảm dần
rồi chọn cặp chưa dùng theo kiểu greedy. Bounding box của track được thay trực tiếp bằng detection
mới; không có dự đoán chuyển động, tối ưu assignment toàn cục hoặc appearance cue.

Cách hiện tại đủ cho một người đứng tương đối yên, nhưng có các giới hạn quan trọng trong cabin:

- Hai người đi cắt nhau hoặc bounding box chồng nhau dễ đổi ID vì greedy chọn cặp tốt nhất cục bộ.
- Khi YOLO miss vài frame, track giữ nguyên box cũ; detection quay lại có thể không còn đủ IoU để
  nối vào track cũ.
- Detection dưới `person_thr` bị bỏ trước khi tracker nhìn thấy, dù đó có thể là người thật đang bị
  che khuất.
- Khi `person_every > 1`, tracker không predict ở frame bỏ YOLO; overlay bị đứng/nhảy.
- `max_missed` đếm lần update thay vì thời gian, nên cùng config nhưng hành vi khác theo FPS.
- Face-to-track được xử lý độc lập từng face bằng containment; nhiều face có thể chọn cùng một track
  và face có thể gắn sai khi hai person box overlap.
- Ghost cache khôi phục ID theo display name; hai resident có cùng tên/greeting có thể bị gộp nhầm.

Spec này thay tracker hiện tại bằng **ByteTrack-lite** viết bằng C++: Kalman motion prediction,
Hungarian assignment, association hai mức confidence, lifecycle theo thời gian, face-to-track một-một
và recognition identity theo `resident_id`. Không thêm person ReID model trong phạm vi này.

## 2. Mục tiêu

1. Giảm ID switch và track fragmentation trong cảnh 1–6 người của cabin.
2. Duy trì cùng track ID qua YOLO miss/che khuất ngắn trong timeout cho phép.
3. Dùng detection confidence thấp để phục hồi track nhưng không tạo false track mới.
4. Predict box ở mọi frame kể cả khi YOLO chạy thưa hơn video.
5. Liên kết face–person một-một và deterministic.
6. Dùng `resident_id` làm khóa identity; display name chỉ phục vụ UI.
7. Giữ tương thích interaction, resident hot reload, audit event và privacy logging.
8. Không làm giảm đáng kể FPS/latency trên Orange Pi A733.
9. Có chế độ rollback sang tracker greedy trong giai đoạn rollout.

## 3. Phạm vi

### Trong scope

- Backend tracker mới: Kalman box state + Hungarian + two-stage ByteTrack association.
- Chuẩn hóa tracker API; tránh trả raw pointer vào vector nội bộ.
- YOLO decode tới low confidence threshold rồi chia high/low trong tracker.
- Lifecycle theo monotonic timestamp: tentative/tracked/lost/removed.
- Predict ở frame không chạy YOLO.
- Face-to-track association một-một.
- Recognition cache và ghost keyed bằng `resident_id`.
- Reconcile tracker với resident catalog hot reload.
- CLI config tối thiểu, startup observability và rollback mode.
- Unit tests, deterministic detection replay, benchmark và live smoke test.

### Ngoài scope

- Thêm person ReID CNN/DeepSORT/BoT-SORT hoặc model NPU mới.
- Train/fine-tune YOLO, SCRFD hoặc recognition model.
- Cross-camera tracking giữa nhiều cabin.
- Persist track state qua process restart.
- Thay đổi schema SQLite residents/cabins nếu không thật sự cần cho config rollout.
- Tracking ngoài vùng camera hoặc suy luận identity khi không có bằng chứng.

## 4. Thuật ngữ

- **High detection**: person detection có score ≥ `person_high_thr`.
- **Low detection**: person detection có `person_low_thr ≤ score < person_high_thr`.
- **Tentative track**: track mới, chưa đủ hit để công bố cho interaction/recognition.
- **Tracked track**: track đã confirm và đang được quan sát hoặc predict ngắn hạn.
- **Lost track**: track đã confirm nhưng chưa được match trong một khoảng thời gian.
- **Removed track**: track hết lost timeout; không còn tham gia geometry association.
- **Reactivation**: Lost track được detection ghép lại và giữ nguyên track ID.
- **Ghost identity**: thông tin identity tối thiểu giữ sau khi track bị remove để nối lại bằng
  `resident_id`; không tham gia geometry association.
- **FrameContext**: `frame_id`, monotonic timestamp, frame dimensions và cờ detector có chạy.

## 5. Yêu cầu chức năng

### R1 — API tracker và compatibility

1. HỆ THỐNG PHẢI cung cấp API tracker không làm lộ reference/pointer có thể invalid khi storage erase
   hoặc reallocate.
2. Mọi consumer PHẢI tham chiếu track bằng `track_id` hoặc immutable `TrackView` theo frame.
3. Trong rollout, HỆ THỐNG PHẢI hỗ trợ `greedy` và `byte` qua `--tracker-mode`.
4. `greedy` PHẢI giữ hành vi cũ đủ để rollback; không cần nhận tính năng mới ngoài adapter API.
5. Nếu không truyền `--person-model`, pipeline SCRFD-only PHẢI tiếp tục chạy không tracker như hiện tại.
6. Subject key của `InteractionManager` PHẢI tiếp tục là track ID ổn định khi tracker bật.

### R2 — Kalman motion prediction

1. Mỗi Byte track PHẢI có state tối thiểu `(cx, cy, w, h, vx, vy, vw, vh)` và covariance.
2. `predict()` PHẢI chạy ở mọi video frame, kể cả frame không chạy YOLO.
3. Transition matrix PHẢI dùng `dt` từ monotonic timestamp, không giả định FPS cố định.
4. Detection match PHẢI correction/update state; frame không detection chỉ predict.
5. Width/height dự đoán PHẢI được clamp dương và box PHẢI được chuẩn hóa trước khi xuất cho UI.
6. Nếu timestamp lùi, không hữu hạn hoặc gap vượt ngưỡng reconnect, velocity PHẢI reset hoặc track
   PHẢI chuyển qua đường recovery an toàn; không để box nhảy vô hạn.
7. Kalman implementation PHẢI deterministic với cùng input sequence.

### R3 — Hungarian assignment và gating

1. HỆ THỐNG PHẢI giải assignment một-một toàn cục giữa tracks và detections.
2. Solver PHẢI hỗ trợ ma trận chữ nhật, empty input và forbidden pairs.
3. Cost cơ sở PHẢI dựa trên predicted-box IoU; implementation có thể thêm normalized center distance
   nếu được chứng minh bằng benchmark cabin.
4. Cặp không vượt association gate PHẢI bị loại, không được Hungarian ép match.
5. Tie-break PHẢI deterministic để cùng dữ liệu tạo cùng track IDs.
6. Không thêm dependency runtime ngoài toolchain/OpenCV hiện có chỉ để giải assignment.

### R4 — ByteTrack association hai vòng

1. YOLO decoder PHẢI trả mọi person detection có score ≥ `person_low_thr` sau NMS.
2. Tracker PHẢI chia detection thành high và low theo `person_high_thr`.
3. Vòng 1 PHẢI ghép Tracked/Lost candidates với high detections bằng Hungarian.
4. Vòng 2 PHẢI ghép các Tracked candidates còn lại với low detections.
5. Chỉ unmatched high detection mới được tạo Tentative track.
6. Low detection KHÔNG được tạo track mới.
7. Lost track match lại trong timeout PHẢI giữ nguyên track ID.
8. Duplicate detections/tracks sau association PHẢI được suppress/resolve deterministic.
9. Detection score và confidence tier PHẢI được giữ trong diagnostics không chứa PII.

### R5 — Lifecycle theo thời gian

1. State machine PHẢI gồm `Tentative`, `Tracked`, `Lost`, `Removed`.
2. Tentative track chỉ chuyển thành Tracked sau `confirm_hits` match hợp lệ.
3. Tentative track miss trước khi confirm PHẢI bị remove mà không tạo interaction event.
4. Tracked track miss PHẢI chuyển Lost theo rule cấu hình.
5. Lost track quá `lost_timeout_ms` PHẢI chuyển Removed.
6. Timeout PHẢI dựa trên monotonic milliseconds; hành vi không được đổi khi FPS đổi.
7. Removed track không được trả về trong `active_tracks()` và không được nhận face association.
8. Track ID trong một process PHẢI tăng/unique; reactivation là ngoại lệ giữ lại ID cũ.

### R6 — Face-to-track association

1. Mỗi frame, HỆ THỐNG PHẢI association toàn bộ faces với toàn bộ eligible person tracks trong một
   bài toán một-một.
2. Face–track candidate PHẢI qua geometric gate: face center trong head region hợp lệ và kích thước
   tương đối không bất thường.
3. Cost PHẢI ưu tiên normalized distance tới expected head center; history frame trước có thể là
   tie-break có giới hạn.
4. Một face không được gắn cho nhiều tracks; một track không được nhận nhiều faces cùng frame.
5. Face không có eligible track PHẢI giữ trạng thái orphan như hiện tại; có thể recognition để hiển
   thị nhưng không được tạo stable interaction subject giả.
6. Association PHẢI deterministic khi input ordering thay đổi.

### R7 — Recognition cache và ghost identity

1. Track PHẢI lưu `resident_id` riêng với `display_name`; identity equality chỉ dùng `resident_id`.
2. `record_recognition()` PHẢI nhận `track_id`, `resident_id`, display label, similarity và timestamp.
3. Unknown PHẢI được biểu diễn bằng `resident_id < 0`, không dùng chuỗi name để quyết định identity.
4. Ghost PHẢI lưu `resident_id`, old track ID, removal timestamp và geometry tối thiểu; không dùng
   display name làm khóa.
5. Ghost inheritance chỉ được phép khi resident ID trùng, ghost chưa hết TTL và không có active track
   khác đang sở hữu resident đó.
6. Nếu recognition conflict với identity đang cache, tracker PHẢI yêu cầu đủ evidence/confirm rule
   trước khi đổi ownership; không đổi ID chỉ từ một frame score thấp.
7. Recognition retry interval hiện tại PHẢI được giữ tương thích, nhưng scheduling nên theo timestamp
   để ổn định theo FPS.
8. Log tracker/ghost KHÔNG được chứa resident name, greeting hoặc embedding.

### R8 — Resident hot reload và interaction

1. Added resident PHẢI invalidate unknown/unrecognized live tracks để recognition lại sớm.
2. Changed/removed resident PHẢI invalidate track đang cache đúng resident ID.
3. Invalidation recognition PHẢI giữ geometry, motion state, track ID và lifecycle state.
4. Ghost liên quan resident changed/removed PHẢI bị purge; policy purge-all hiện tại được phép trong
   bản đầu nếu đơn giản và an toàn.
5. `InteractionManager` cooldown/history không được reset cho resident không bị ảnh hưởng.
6. Track mapping và interaction session PHẢI reconcile tại cùng frame-boundary safe point như hot
   reload hiện tại.
7. Không được tạo duplicate confirmed event chỉ vì tracker backend chuyển state Tracked↔Lost.

### R9 — Configuration và backward compatibility

1. `--person-thr` PHẢI tiếp tục là high threshold để không phá CLI hiện tại.
2. HỆ THỐNG PHẢI thêm `--person-low-thr`, với validate `0 ≤ low < high ≤ 1`.
3. HỆ THỐNG PHẢI thêm `--track-lost-ms` hoặc cung cấp migration rõ ràng từ `--track-max-miss`.
4. `--track-iou` PHẢI tiếp tục điều khiển association gate vòng chính.
5. HỆ THỐNG PHẢI thêm `--tracker-mode greedy|byte` trong giai đoạn rollout.
6. Giá trị invalid PHẢI báo lỗi config rõ ràng; không silently tạo tracker nguy hiểm.
7. Startup log PHẢI ghi mode và numeric tracker config hiệu lực, không chứa PII.
8. Tracker settings vẫn là deployment/admin config; không tự mở cho web patch trong spec này.

### R10 — RTSP/reconnect và shutdown

1. Khi RTSP mất frame, tracker không được spin hoặc dùng wall-clock âm.
2. Sau reconnect gap lớn, velocity stale PHẢI được reset trước khi tiếp tục association.
3. Shutdown bằng q/ESC, SIGINT hoặc SIGTERM PHẢI dừng tracker cùng pipeline và không làm join treo.
4. Tracker không được spawn background thread mới; toàn bộ update chạy trên main frame thread.

### R11 — Observability và privacy

1. Startup log PHẢI có mode, thresholds, association gate, lost timeout và confirm hits.
2. Runtime INFO chỉ log transition/health có ích và phải rate-limit nếu lặp.
3. Per-frame association details chỉ được phép ở DEBUG/TRACE.
4. Log chỉ dùng track ID/resident ID; không log name/greeting/embedding/ảnh/RTSP URL.
5. Benchmark summary PHẢI tách tracker predict/associate nếu cần đo regression nhưng không làm đổi
   format vận hành nếu chưa có migration tài liệu.

## 6. Yêu cầu kiểm thử và nghiệm thu

### R12 — Unit và deterministic replay

1. Hungarian PHẢI có test square/rectangular/empty/forbidden/tie.
2. Kalman PHẢI có test đứng yên, constant velocity, missed detection, variable `dt`, invalid gap.
3. Tracker PHẢI có test high/low association, lifecycle, reactivation, duplicate suppression.
4. Face association PHẢI có test overlap, orphan, one-to-one và input-order invariance.
5. Identity PHẢI có test hai residents trùng display name không merge và cùng resident re-entry có thể
   inherit đúng ID.
6. Hot reload/privacy tests hiện tại PHẢI tiếp tục pass sau API migration.
7. HỆ THỐNG PHẢI có replay fixture dạng text/JSON không chứa ảnh/PII để tái hiện crossing/occlusion.

### R13 — Performance và live validation

1. `make -j4` PHẢI build toàn bộ targets không warning mới.
2. Full unit suite PHẢI pass; test không liên quan bị block phải được ghi rõ, không che lỗi tracker.
3. Với ≤10 detections, tracker p95 mục tiêu <1 ms trên Orange Pi A733.
4. End-to-end FPS mục tiêu không giảm quá 2% so với baseline trên cùng video/config.
5. Deterministic crossing/occlusion fixtures PHẢI không có ID switch ngoài expected annotation.
6. Low detections không được tự tạo track mới.
7. Trên bộ clip cabin, ID switches và fragmentation PHẢI thấp hơn greedy baseline; mục tiêu giảm ≥50%
   trước khi đổi default.
8. Không có duplicate greeting/event do tracker trong bộ clip nghiệm thu.
9. Nếu có annotation đầy đủ, báo cáo IDF1/HOTA trước–sau; nếu không, báo IDSW, fragments, duplicate
   events, FPS và p95 tracker latency.

## 7. Rollout và rollback

1. Tracker mới PHẢI merge với default `greedy` ở giai đoạn đầu.
2. Detection replay PHẢI chạy A/B cả hai backend từ cùng input.
3. Một cabin thử nghiệm PHẢI chạy `byte` qua ít nhất một vòng smoke/soak đã thống nhất.
4. Chỉ đổi default sang `byte` sau khi R13 đạt.
5. `--tracker-mode greedy` PHẢI được giữ ít nhất một chu kỳ release sau khi đổi default.
6. Rollback không được yêu cầu DB migration hoặc xóa resident/event data.

## 8. Tiêu chí hoàn thành spec

Spec hoàn thành khi:

- Backend ByteTrack-lite chạy mặc định và greedy fallback đã qua rollback test.
- Mọi unit/replay test pass.
- Build toàn bộ target sạch.
- Báo cáo A/B trên Orange Pi chứng minh latency/FPS trong giới hạn và association tốt hơn baseline.
- Live RTSP smoke xác nhận crossing, occlusion, reconnect, recognition cache, hot reload và shutdown.
- Tài liệu CLI/README/development plan được cập nhật.

