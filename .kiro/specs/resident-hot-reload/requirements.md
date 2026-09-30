# Requirements — Resident Hot Reload

**Spec ID**: `resident-hot-reload`  
**Phụ thuộc**: `resident-db-layer`, Phương án A enroll (`add_person --ext-id ... --json`),
`main-cpp-refactor`  
**Liên quan**: `edge-core-uds` đợt b (đường dài hạn), `cabin-runtime-config`

## 1. Bối cảnh & vấn đề

`face_recog_app` hiện gọi `ResidentDB::load_active()` đúng một lần lúc khởi động, xây
`MatchEngine` và `resident_by_id` trong RAM rồi giữ nguyên cho đến khi thoát. Phương án A enroll
chạy `add_person` ở tiến trình riêng: ảnh/embedding được ghi thành công vào SQLite trong khi app
đang chạy, nhưng app không thấy resident mới cho đến khi restart.

Test thực tế ngày 30/09 đã xác nhận `add_person` có thể enroll đồng thời với app: cả hai tiến trình
cùng mở `/dev/vipcore`, app có thể khựng ngắn (state R/D) nhưng không crash. Sau khi `add_person`
thoát, SQLite có dữ liệu mới; phần còn thiếu duy nhất là cập nhật catalog nhận diện trong RAM.

Mục tiêu của spec: sau khi transaction enroll commit, app tự phát hiện thay đổi và nạp catalog mới
trong tối đa khoảng một chu kỳ poll, **không restart**, **không đổi schema**, không làm hỏng catalog
đang dùng nếu reload thất bại.

## 2. Phạm vi

### Trong scope

1. Poll thay đổi resident/embedding trong SQLite khi chạy `--resident-db`.
2. Mặc định poll mỗi 1000 ms; CLI `--resident-reload-ms N` cho phép chỉnh, `0` = tắt.
3. Phân biệt thay đổi catalog thật với ghi audit/runtime (`match_events`, `last_seen_at`,
   `match_count`) để tránh rebuild matcher vô ích.
4. Load residents + embeddings bằng một SQLite snapshot nhất quán, qua reader read-only timeout ngắn.
5. Biến batch ghi của `add_person` (upsert/delete/add embeddings) thành **một transaction atomic**;
   `ok:true` chỉ sau COMMIT, lỗi bất kỳ phải ROLLBACK.
6. Validate catalog mới và chỉ swap vào runtime khi toàn bộ reload thành công.
7. Invalidate recognition/ghost/session cache liên quan để resident vừa enroll có thể được thử nhận
   diện ở frame kế tiếp, không phải chờ `recog_unknown_retry`.
8. Poll vẫn đúng nhịp khi camera không có frame bằng timed wait tới reload deadline.
9. Unit/integration test không NPU cho transaction/detection/snapshot/swap; một smoke test NPU có
   kiểm soát cho luồng app-running + `add_person`.

### Ngoài scope

- Enroll model inference trong cùng process; Phương án A vẫn spawn `add_person` và có thể gây NPU
  contention ngắn.
- Hot-reload camera hoặc cabin AI config.
- Thay đổi schema/migration, trigger hoặc bảng revision mới.
- `.fdb` test/dev hot-reload.
- UDS `0x2002 register request`; đây là kiến trúc cuối ở `edge-core-uds` đợt b.
- Tối ưu FPS YOLO/SCRFD và thermal.

## 3. Thuật ngữ

- **Catalog**: tập active residents, metadata dùng runtime và mọi embedding của họ.
- **Catalog fingerprint**: hash ổn định của các trường catalog ảnh hưởng runtime; không gồm
  `match_events`, `last_seen_at`, `match_count`.
- **Candidate**: catalog mới đã load vào biến tạm nhưng chưa được áp dụng.
- **Active catalog**: `MatchEngine` + `resident_by_id` đang phục vụ frame loop.
- **Affected track**: track unknown/unrecognized khi có resident mới, hoặc track đang map tới
  resident bị thay đổi/xóa/deactivate.

## 4. Yêu cầu chức năng

### R1 — Bật/tắt và chu kỳ kiểm tra

1. Hot reload CHỈ hoạt động khi app chạy với `--resident-db`; nhánh `.fdb` PHẢI giữ nguyên.
2. CLI PHẢI có `--resident-reload-ms N`, mặc định `1000`; `N >= 0`, `0` tắt hot reload.
3. App KHÔNG được query SQLite ở mọi frame; chỉ kiểm tra khi hết chu kỳ theo monotonic clock.
4. Nếu capture không có frame/RTSP bị ngắt, wait của frame loop PHẢI thức theo reload deadline để
   poll vẫn chạy; không được chờ frame vô hạn làm mất cam kết 1000 ms.
5. Giá trị âm PHẢI bị từ chối như bad argument hoặc clamp về 0 kèm cảnh báo; hành vi phải nhất quán
   với parser hiện có và được test.
6. Log khởi động PHẢI ghi hot reload enabled/disabled và interval, không log PII.

### R2 — Phát hiện đúng thay đổi catalog

1. App PHẢI dùng một SQLite connection reader riêng để quan sát commit từ `add_person` và tránh dùng
   đồng thời connection đang phục vụ async writer.
2. Mỗi chu kỳ, app NÊN đọc `PRAGMA data_version` trên reader connection làm bộ lọc rẻ.
3. Khi `data_version` không đổi, app KHÔNG được load/rebuild catalog.
4. Khi `data_version` đổi, app PHẢI load candidate và tính content fingerprint.
5. Ghi mới vào `match_events` hoặc cập nhật `last_seen_at`/`match_count` KHÔNG được làm thay đổi
   fingerprint và KHÔNG được swap/rebuild `MatchEngine`.
6. Các thay đổi sau PHẢI được phát hiện: thêm resident active, thêm/replace/xóa embedding, đổi
   metadata runtime (`name`, `greeting_name`, `home_floor`, `language`, `role`, `ext_id`), và
   active/deactivate/delete resident.
7. Không dùng mtime của file `.db`/`.db-wal` làm nguồn duy nhất vì WAL/audit tạo false positive.

### R3 — Snapshot nhất quán và lỗi an toàn

1. `add_person` PHẢI bọc upsert resident, optional delete embeddings và toàn bộ add embeddings trong
   một `BEGIN IMMEDIATE`/COMMIT; bất kỳ write fail nào PHẢI ROLLBACK và không trả `ok:true`.
2. Hai SELECT residents và embeddings PHẢI chạy trong cùng một read transaction/snapshot.
3. Query PHẢI có thứ tự ổn định (`ORDER BY`) để fingerprint không đổi vì thứ tự hàng.
4. Reader PHẢI mở read-only/no-migration, không spawn async writer, với busy timeout ngắn (không để
   poll trên main thread đứng tối đa 5 giây).
5. Nếu DB busy, SELECT lỗi, transaction lỗi, vector blob hỏng hoặc catalog candidate không hợp lệ,
   app PHẢI giữ nguyên active catalog và tiếp tục nhận diện bằng dữ liệu cũ.
6. Reload lỗi PHẢI được retry ở chu kỳ sau dù `data_version` không tăng thêm; không được đánh dấu
   version lỗi là đã áp dụng.
7. Khi reader reconnect, app PHẢI bỏ data_version cũ và force full snapshot vì `data_version` là
   connection-local.
8. Một reload thất bại KHÔNG được crash app, clear matcher hoặc khiến mọi người thành unknown.

### R4 — Validate và áp dụng atomically

1. Candidate PHẢI đọc `embeddings.id`, declared `dim` và blob byte length; yêu cầu blob non-NULL,
   `nbytes == dim*sizeof(float)` và `dim == --recog-dim` cho **mọi** embedding.
2. Mọi float PHẢI finite, vector có norm > epsilon và nằm trong tolerance L2-normalized đã định;
   NaN/Inf/zero/trailing bytes PHẢI làm candidate bị từ chối toàn bộ.
3. Sau mỗi SELECT, app PHẢI kiểm tra terminal `sqlite3_step` status là `SQLITE_DONE` trước COMMIT.
4. Nếu candidate có embedding nhưng sau build không có vector hợp lệ, reload PHẢI bị từ chối.
5. App PHẢI build `MatchEngine` tạm và `resident_by_id` tạm trước khi thay active catalog.
6. Chỉ khi load + validate + build thành công, app mới swap toàn bộ candidate tại một safe point
   giữa hai frame.
7. Frame đang xử lý PHẢI dùng trọn một catalog generation; không được match bằng matcher mới nhưng
   đọc metadata map cũ hoặc ngược lại.
8. Reload không chạy NPU và không reload model; chỉ đọc SQLite + build index CPU/RAM.
9. Catalog rỗng hợp lệ PHẢI được áp dụng và log cảnh báo “everyone unknown”, không crash.

### R5 — Cache tracker và interaction

1. Sau reload có resident mới, các track đang unknown/chưa recognized PHẢI được đánh dấu cần
   recognition ở frame kế tiếp; không chờ `recog_unknown_retry` 30 frame.
2. Khi metadata/embedding của một resident thay đổi, track map tới resident đó PHẢI bị invalid và
   recognition lại; track của resident không đổi KHÔNG nên bị reset vô ích.
3. Khi resident bị deactivate/delete, mọi `track_resident_id` trỏ tới ID đó PHẢI bị xóa và track
   tương ứng PHẢI recognition lại.
4. Invalidate recognition PHẢI giữ nguyên person bbox, track ID và tuổi track; không reset toàn bộ
   person tracker.
5. Ghost identity cache PHẢI được purge/reconcile khi catalog đổi; ghost không được giữ name/identity
   của resident đã đổi/xóa. Nếu chưa có resident_id trong Ghost, v1 PHẢI purge toàn bộ ghosts trên
   content reload (an toàn hơn kế thừa identity cũ).
6. Interaction session của affected subject PHẢI được reconcile/drop mà KHÔNG xóa cooldown history
   toàn cục; reload không được tự phát thêm một `matched`/floor-action trùng cho resident không đổi.
7. Nhánh không tracker (`SCRFD-only`) vốn recognition mỗi frame: thêm resident không liên quan KHÔNG
   được drop subject key 0. Chỉ reconcile/drop key 0 nếu SessionView hiện tại map tới resident thuộc
   `changed/removed`; unknown/DETECTING session tự dùng matcher mới ở frame kế tiếp.
8. `track_resident_id`, tracker/ghost cache và interaction state PHẢI được cập nhật ở cùng safe point
   với catalog swap.
9. Mọi log/printf tracker hiện chứa display name PHẢI được thay bằng logger không PII trong phạm vi
   task này; reload không được kích hoạt đường log lộ tên.

### R6 — Quan sát được và privacy

1. Reload thành công PHẢI log: generation, số residents, số embeddings, reload latency ms và số
   affected tracks.
2. Reload không đổi nội dung (chỉ audit/data_version đổi) chỉ được DEBUG log hoặc im lặng.
3. Reload thất bại PHẢI WARN/ERROR với loại lỗi và lần retry; rate-limit nếu lỗi lặp lại.
4. Log TUYỆT ĐỐI KHÔNG chứa `name`, `greeting_name`, apartment, vector hoặc đường dẫn ảnh; chỉ dùng
   resident count, embedding count, generation, resident_id/track_id khi thật sự cần.
5. Có counters tối thiểu: polls, data-version changes, content changes, reload success/failure.

### R7 — Hiệu năng và độ ổn định

1. Poll không đổi PHẢI là thao tác nhẹ và không làm giảm FPS đo được ngoài nhiễu benchmark.
2. Reader busy timeout PHẢI ngắn (mục tiêu 25–100 ms); lỗi/busy trả control cho frame loop và retry,
   không block video bằng timeout mặc định 5000 ms.
3. Full load/hash/build chỉ chạy khi `data_version` đổi; swap chạy trên main thread tại frame boundary
   để tránh data race.
4. Reload không tạo thread vô hạn, không giữ lock qua NPU inference và không chặn async event writer.
5. Với catalog nhỏ hiện tại, resident vừa enroll PHẢI có trong active matcher trong tối đa
   `resident_reload_ms + thời gian một reload thành công` sau commit, kể cả khi camera không có frame.
6. App phải shutdown sạch; reader connection đóng trước khi SQLite/NPU teardown hoàn tất.

### R8 — Tương thích và không hồi quy

1. Không thay `db/schema.sql`, không bump `schema_version`.
2. Startup load hiện tại, match max-cosine multi-embedding, interaction/cooldown, EdgeClient và audit
   behavior PHẢI giữ nguyên khi catalog không đổi.
3. `--resident-reload-ms 0` PHẢI cho hành vi tương đương hiện tại (load một lần lúc startup).
4. `add_person`/web/Mongo contract không đổi, nhưng `add_person` PHẢI đổi nội bộ sang atomic write
   transaction để một reload không bao giờ nhận resident với zero/partial embeddings.
5. CLI precedence/config camera không thuộc spec và không bị thay đổi.

## 5. Tiêu chí chấp nhận end-to-end

1. Start app với DB có resident A; xác nhận log initial generation.
2. Giữ app chạy, dùng `add_person` enroll resident B vào đúng DB.
3. `add_person` trả `ok:true`; trong vòng khoảng 1 giây app log một reload success, không restart/PID
   không đổi.
4. Resident B được match ở các frame tiếp theo; nếu B đang đứng trước camera từ trước khi enroll,
   unknown track được recognition lại ngay, không chờ 30 frame.
5. App tiếp tục match A đúng; không phát duplicate confirmed event chỉ do reload.
6. Chỉ ghi `match_events`/`touch_resident` không làm tăng catalog generation.
7. Dừng frame input/RTSP outage trong lúc external commit: poll vẫn chạy theo deadline và generation
   vẫn tăng không cần đợi camera hồi phục.
8. Inject embedding sai declared dim, trailing bytes, NULL/NaN/Inf/zero hoặc DB busy: app log reload
   failure, vẫn match bằng generation cũ, rồi retry.
9. Benchmark sạch trước/sau với catalog không đổi không có giảm FPS đáng kể.
10. Test replace nhiều embeddings: generation chỉ áp catalog trước hoặc sau nguyên batch, không bao
    giờ quan sát zero/partial embeddings.

## 6. Quyết định đã chốt

- Phương án trung gian: **poll SQLite 1000 ms**, không signal từ Node, tránh coupling với
  `edge_elevator/server.js` do đội IoT quản lý.
- Không đổi schema; dùng `data_version` + content fingerprint.
- `add_person` ghi atomic một transaction; `ok:true` chỉ sau COMMIT.
- Reader read-only/no-migration, busy timeout ngắn + reconnect force full snapshot.
- Poll deadline độc lập frame arrival (timed wait khi camera stall).
- Candidate-build-then-swap; lỗi giữ catalog cũ.
- Invalidate có chọn lọc, purge ghost cache và giữ cooldown.
- UDS đợt b vẫn là đường dài hạn để loại bỏ process/NPU contention của Phương án A.
