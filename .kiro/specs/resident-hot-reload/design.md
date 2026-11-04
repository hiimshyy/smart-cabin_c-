# Design — Resident Hot Reload

**Spec ID**: `resident-hot-reload`

Tài liệu yêu cầu: [requirements.md](requirements.md)

## Overview

Thiết kế này hiện thực [requirements.md](requirements.md): khi `face_recog_app` chạy với
`--resident-db`, app tự phát hiện commit enroll mới của `add_person` trong SQLite và nạp catalog nhận
diện mới (`MatchEngine` + `resident_by_id`) mà không restart, không đổi schema. Nếu reload lỗi, app
giữ catalog đang dùng.

Quyết định chính:

- Poll SQLite mỗi 1000 ms (`--resident-reload-ms N`, `0` tắt) trên một reader connection read-only
  riêng, busy timeout ngắn. Không có signal từ Node/web.
- `PRAGMA data_version` là bộ lọc rẻ; content fingerprint quyết định có reload hay không, nên ghi
  audit (`match_events`, `last_seen_at`, `match_count`) không gây rebuild matcher.
- Residents và embeddings được đọc trong cùng một read transaction; `add_person` ghi cả batch trong
  một `BEGIN IMMEDIATE`/COMMIT.
- Candidate được validate và build ngoài DB transaction, rồi matcher và metadata map được swap cùng
  lúc tại frame boundary trên main thread. Lỗi bất kỳ giữ generation cũ và retry ở chu kỳ sau.
- Sau swap, chỉ invalidate tracker/session cache bị ảnh hưởng, purge ghost cache và giữ cooldown.
- Frame loop dùng timed wait theo reload deadline để poll vẫn chạy khi camera không có frame.

Ngoài phạm vi: các mục "Ngoài scope" của requirements (enroll inference trong cùng process,
hot-reload camera/cabin config, schema migration, `.fdb` hot-reload, UDS `0x2002`).

## Architecture

### Luồng dữ liệu

```
Web / edge_elevator
        │ spawn add_person
        ▼
SQLite WAL: residents + embeddings ───────────────────────────────┐
        ▲                                                          │
        │ commit                                                   │ poll 1000 ms
        │                                                          ▼
add_person (NPU, process ngắn)                         ResidentCatalogReloader
                                                       dedicated reader connection
                                                          │ PRAGMA data_version
                                                          │ snapshot load + fingerprint
                                                          │ candidate validate/build
                                                          ▼
                                               frame boundary atomic swap
                                          MatchEngine + resident_by_id generation N
                                                          │
                                                          ├─ invalidate affected tracker caches
                                                          └─ reconcile affected interaction sessions
```

`add_person` không cần biết PID/socket của app. Commit SQLite là tín hiệu. Điều này tránh sửa thêm
`edge_elevator/server.js` (file thuộc đội IoT và từng bị deploy ghi đè).

### Timed poll và atomic swap trong main loop

Main hiện `slot.cv_new.wait(...)` vô hạn; nếu RTSP mất frame thì code đặt poll sau wait sẽ không chạy.
Thay bằng `wait_until(next_reload_deadline)` khi reload enabled:

```text
lock capture slot
wait_until(min(next_reload_deadline, shutdown), predicate frame/stop)
unlock
if stop -> break
if reload deadline reached -> poll/reload tại safe point
if không có frame mới -> continue (quay lại timed wait, không chạy NPU)
copy latest frame và xử lý bình thường
```

Như vậy poll chạy mỗi ~1000 ms cả khi camera outage hoặc nguồn <1 FPS. Khi reload disabled dùng wait
vô hạn như behavior cũ. Cần tách helper/timepoint rõ để không busy-loop khi poll/reopen lỗi.

Swap matcher/metadata chỉ tại frame boundary, trước preprocess/NPU hoặc sau khi frame trước hoàn tất;
không chạy giữa match và metadata lookup:

```cpp
ReloadOutput out;
if (reloader.poll(now_ms(), &out) == Reloaded) {
    // all on main thread: no mutex needed for these objects
    match_engine = std::move(out.matcher);
    resident_by_id = std::move(out.resident_by_id);
    reconcile_runtime_caches(out.diff);
    reloader.commit_generation(out);
}
```

Matcher và metadata map luôn swap trong cùng block. Async DB writer không truy cập chúng. EdgeClient
chỉ nhận dữ liệu đã copy khi event phát, không giữ pointer vào map.

Reload có thể gây một frame dài hơn; reader busy timeout ngắn giới hạn pause. Không giữ DB transaction
trong lúc build/hash/swap: load vectors trong snapshot, COMMIT sớm, rồi build candidate ngoài
transaction.

### Threading và locking

- Poll/load/build/swap: main frame thread.
- Dedicated catalog reader: chỉ main frame thread dùng.
- Event writer: connection hiện tại + writer thread riêng về mặt trách nhiệm runtime.
- Capture/display/EdgeClient: không đọc MatchEngine/resident map trực tiếp.
- Không mutex quanh NPU; `add_person` vẫn là process riêng và driver tự serialize/contend như test.
- Không giữ SQLite read transaction qua NPU inference.

## Components and Interfaces

### Bản đồ file

| File | Thay đổi |
|---|---|
| `src/resident_catalog.{h,cpp}` (mới) | Fingerprint, generation, candidate load/validate/build, diff affected resident IDs |
| `src/resident_db.{h,cpp}` | `open_readonly()` timeout ngắn, `data_version()`, `load_active_snapshot()` transaction + ORDER BY + strict blob metadata |
| `src/add_person.cpp` | Atomic batch: begin → upsert/delete/add all embeddings → commit; rollback mọi lỗi |
| `src/match_engine.h` | Giữ move/swap semantics rõ ràng; có thể thêm `swap()` nếu cần |
| `src/tracker.{h,cpp}` | API invalidate recognition affected tracks, purge ghosts; bỏ printf chứa name |
| `src/interaction.{h,cpp}` | API drop/reconcile session theo subject key nhưng giữ cooldown history |
| `src/video_io.*` / `src/main.cpp` | Timed wait theo reload deadline khi không có frame |
| `src/app_config.{h,cpp}` | `--resident-reload-ms`, mặc định 1000; 0 disable |
| `Makefile` | Thêm module mới vào `DB_SRCS`/app-only phù hợp |
| `tests/test_resident_catalog.cpp` | Unit/property tests fingerprint, no-op audit, candidate failure, diff |
| `tests/test_tracker.cpp` hoặc test hiện có | Invalidation giữ track geometry/ID |
| `tests/test_interaction.cpp` | Drop affected sessions nhưng giữ cooldown |
| `tests/run_tests.sh` | Build/run test mới không NPU |

Không sửa `db/schema.sql` và không thêm dependency.

### Reader connection và snapshot

#### Vì sao cần connection riêng

`ResidentDB` của realtime app có async writer thread ghi `match_events` và `touch_resident`. Dùng cùng
`sqlite3*` để poll/read đồng thời làm phức tạp transaction/thread safety. Thiết kế thêm API reader
chuyên biệt, không dùng `ResidentDB::open()` hiện tại vì hàm đó có busy timeout 5000 ms, chạy WAL/
migration và có thể ghi:

```cpp
ResidentDB catalog_reader;
catalog_reader.open_readonly(db_path, /*busy_timeout_ms=*/50);
```

`open_readonly()` dùng `sqlite3_open_v2(SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)`, không apply
schema/migration, không đổi journal mode, không spawn writer, đặt `PRAGMA query_only=ON` và busy
timeout mục tiêu 25–100 ms. Open lỗi không làm app mất active catalog: reloader retry với exponential
backoff 1–10 s. Sau mọi reconnect, xóa accepted data version và **force full snapshot**, vì
`PRAGMA data_version` là số connection-local, không được so trực tiếp giữa hai connection đời khác.

Reader chỉ main frame thread sử dụng; mọi write API bị cấm theo contract.

#### `data_version()`

```cpp
std::optional<int64_t> ResidentDB::data_version() const;
// PRAGMA data_version; trên chính reader connection
```

SQLite tăng giá trị khi **connection khác** commit. Vì async writer và `add_person` đều là connection
khác, data_version bắt được cả hai. Audit commit là false positive có chủ ý; fingerprint loại nó ở
bước sau.

#### `load_active_snapshot()`

```cpp
bool ResidentDB::load_active_snapshot(
    std::vector<Resident>& residents,
    std::vector<EmbeddingRow>& embeddings);
```

Pseudocode:

```text
BEGIN;                         -- deferred read transaction
SELECT runtime resident fields
  FROM residents
 WHERE active=1
 ORDER BY id;

SELECT e.id, e.resident_id, e.source, e.dim, e.vector, length(e.vector)
  FROM embeddings e
  JOIN residents r ON r.id=e.resident_id
 WHERE r.active=1
 ORDER BY e.resident_id, e.id;
COMMIT;
```

`EmbeddingRow` được mở rộng nội bộ với `id`, `declared_dim`, `blob_nbytes`, `blob_valid` (không đổi
schema/API business). Loader yêu cầu non-NULL blob và ghi nhận exact byte length; validator kiểm tra
`nbytes == declared_dim*sizeof(float)` trước khi dùng vector. Sau mỗi loop rows, phải xác nhận terminal
`sqlite3_step == SQLITE_DONE`; status khác là lỗi snapshot.

Mọi error → finalize statement, `ROLLBACK`, clear candidate outputs, return false. Transaction đọc
đảm bảo resident và embeddings cùng một SQLite snapshot dù writer commit giữa hai SELECT. Không dùng
`BEGIN IMMEDIATE` vì reader không cần write lock.

Để test concurrency deterministic, implementation cung cấp test-only phase hook/barrier (compile
macro hoặc callback injectable) chạy sau SELECT residents và trước SELECT embeddings. Production
build không có hook/no-op. Test writer chờ barrier rồi commit, chứng minh cả hai SELECT vẫn thuộc cùng
snapshot; không dùng race/sleep flaky.

#### Atomic write contract của `add_person`

Read snapshot chỉ nhất quán tại một commit point; nó không thể biến nhiều autocommit thành một enroll
atomic. Vì vậy `add_person`, sau khi đã extract toàn bộ embeddings ngoài transaction, PHẢI ghi:

```text
BEGIN IMMEDIATE
→ upsert resident metadata
→ nếu --replace: delete old embeddings
→ add toàn bộ new embeddings
→ nếu written != expected: ROLLBACK, JSON ok:false
→ COMMIT
→ chỉ sau COMMIT mới JSON ok:true
```

Mọi early return sau BEGIN dùng guard RAII/explicit rollback. `--merge` cũng commit cả batch một lần.
Nhờ vậy poll chỉ thấy generation trước hoặc generation sau, không bao giờ resident zero/partial
embeddings trong lúc replace. Web/Mongo request schema không đổi.

### `ResidentCatalogReloader`: thuật toán poll/reload

```cpp
class ResidentCatalogReloader {
public:
    bool initialize(ResidentDB& reader, int recog_dim,
                    std::vector<Resident> initial_residents,
                    std::vector<EmbeddingRow> initial_embeddings);

    enum class PollResult { NoDbChange, NoCatalogChange, Reloaded, Failed };
    PollResult poll(double now_ms, ReloadOutput* out);

private:
    std::optional<int64_t> accepted_data_version_; // reset on reader reconnect
    bool force_full_snapshot_ = true;
    bool pending_retry_ = false;
    double next_poll_ms_ = 0;
    double next_reopen_ms_ = 0;
    int reopen_backoff_ms_ = 1000;       // max 10000
    int interval_ms_ = 1000;
    ResidentCatalog active_summary_; // fingerprint/digests/generation; matcher owned by main or output
};
```

Luồng mỗi interval:

```text
1. nếu reader đóng/lỗi: thử `open_readonly` theo backoff; open thành công -> force_full_snapshot=true,
   accepted_data_version reset (không so version giữa connection đời khác)
2. read current PRAGMA data_version
3. nếu không force/pending và bằng accepted_data_version -> NoDbChange
4. load_active_snapshot() vào vectors tạm
5. tính fingerprint + per-resident digests
6. nếu fingerprint == active fingerprint:
      accepted_data_version = current
      force=false; pending=false
      NoCatalogChange       (audit/touch only)
7. validate exact blob/dim/finiteness/norm cho toàn bộ embeddings
8. build MatchEngine candidate + resident map candidate
9. nếu fail:
      KHÔNG cập nhật accepted_data_version
      pending_retry=true
      giữ active catalog; Failed
10. tính CatalogDiff
11. trả ReloadOutput candidate cho main
12. main swap tại frame boundary; sau swap mới mark accepted + generation++
```

Điểm 8 quan trọng: nếu DB chỉ thay đổi một lần nhưng reload đúng lúc busy/error, data_version có thể
không tăng lần nữa. Không advance accepted version để chu kỳ sau retry.

### Cache reconciliation

#### Tracker API

Dự kiến thêm:

```cpp
struct InvalidatedTracks {
    std::vector<int> subject_keys;
    int count = 0;
};

InvalidatedTracks Tracker::invalidate_recognition_cache(
    const std::set<int64_t>& changed_or_removed_residents,
    const std::map<int, int64_t>& track_resident_id,
    bool invalidate_unknown);
```

Cho affected live tracks:

```text
name.clear()
match_sim = -1
last_recog_frame = -1
```

Không đổi bbox, state person tracker, `id`, hit/miss counters. `needs_recog()` sẽ trả true ở frame kế.

- `diff.added` không có track mapping cũ → invalidate unknown/unrecognized tracks.
- `diff.changed|removed` → invalidate track đang map tới các resident IDs đó.
- Xóa entry tương ứng khỏi `track_resident_id`.

`Tracker::Ghost` hiện chỉ giữ display `name`, không có resident_id nên không thể reconcile chọn lọc an
toàn (đặc biệt duplicate names). V1 của spec PHẢI purge toàn bộ `ghosts_` mỗi khi fingerprint catalog
đổi. Có thể nâng Ghost lưu resident_id trong refactor sau, nhưng không trì hoãn correctness.

Ngoài ra `Tracker::try_inherit_identity()` hiện có `printf` chứa name. Task này PHẢI thay bằng logger
không PII (track_id/resident_id nếu có), và test source/log không lộ display name.

#### Interaction API

Không gọi `interaction.reset()` vì hàm đó xóa cả `last_confirmed_ms_` và có thể gây duplicate action.
Thêm API hẹp:

```cpp
void InteractionManager::drop_sessions(const std::vector<int>& subject_keys);
```

Hàm chỉ erase `sessions_` theo subject key, **giữ `last_confirmed_ms_`**. Cooldown toàn cục còn hiệu
lực. Subject affected tạo session mới và có thể match catalog mới; subject không affected giữ trạng
thái hiện tại.

SCRFD-only mode không có tracker và vốn recognition ở mọi frame. Vì vậy:

- `diff.added` không drop key 0; DETECTING/unknown session tự nhận MatchResult mới ở frame kế.
- Chỉ drop/reconcile key 0 nếu `SessionView.resident_id` hiện tại nằm trong `diff.changed|removed`.
- Session confirmed của resident A phải giữ nguyên khi chỉ thêm/thay resident B, kể cả cooldown A đã
  hết, để reload không tạo duplicate action.

#### Thứ tự reconcile

```text
swap matcher + resident map
→ xác định/invalidate affected tracker entries
→ erase affected track_resident_id mappings
→ drop affected interaction sessions (preserve cooldown)
→ frame tiếp theo recognition bằng generation mới
```

## Data Models

### Runtime catalog

```cpp
struct CatalogFingerprint {
    uint64_t hash = 0;             // deterministic FNV-1a 64 hoặc equivalent
    size_t resident_count = 0;
    size_t embedding_count = 0;

    bool operator==(const CatalogFingerprint&) const;
};

struct ResidentDigest {
    int64_t resident_id = -1;
    uint64_t metadata_hash = 0;
    uint64_t embeddings_hash = 0;
};

struct ResidentCatalog {
    uint64_t generation = 0;
    CatalogFingerprint fingerprint;
    std::map<int64_t, ResidentDigest> digests;
    std::map<int64_t, Resident> resident_by_id;
    MatchEngine matcher;
};

struct CatalogDiff {
    std::set<int64_t> added;
    std::set<int64_t> changed;
    std::set<int64_t> removed;
};
```

Fingerprint bao gồm mọi dữ liệu ảnh hưởng runtime:

- Resident: `id`, `name`, `apartment`, `home_floor`, `language`, `greeting_name`, `role`, `ext_id`.
- Embedding: `resident_id`, `source`, kích thước vector và **bytes float của vector**.

Không bao gồm `last_seen_at`, `match_count`, `match_events`, timestamps không dùng runtime. PII được
hash trong RAM nhưng không log/serialize.

`ResidentDigest` cho phép xác định resident nào changed để invalid cache có chọn lọc.

### Điều kiện hợp lệ của candidate

Một candidate hợp lệ khi:

1. Snapshot load thành công và mỗi SELECT kết thúc bằng `SQLITE_DONE`.
2. Mỗi embedding có owner active tồn tại, blob non-NULL, declared dim đúng `recog_dim`, và
   `blob_nbytes == declared_dim*sizeof(float)` (không chấp nhận trailing 1–3 bytes).
3. Mọi float `std::isfinite`, norm > epsilon; norm L2 phải thỏa
   `abs(norm - 1.0) <= 0.05` vì matcher giả định normalized.
4. `MatchEngine::build()` tạo đúng số vector bằng số embedding candidate.
5. Catalog rỗng (`0/0`) được phép; catalog có embedding nhưng matcher 0 vector không được phép.

Nếu một vector sai, từ chối **toàn bộ generation**, không áp catalog một phần. Log loại lỗi, row id/
resident_id nếu cần, không log metadata PII/vector.

## Correctness Properties

*Property là đặc tính phải đúng trên mọi lần chạy hợp lệ của hệ thống, tức một phát biểu hình thức
về việc hệ thống phải làm gì. Property là cầu nối giữa đặc tả đọc được và kiểm chứng tự động.*

Các Property 1, 2, 6 áp dụng cho logic thuần trong `resident_catalog`; Property 3, 7, 8 cho
`ResidentCatalogReloader`; Property 4, 5 cần hai SQLite connection; Property 10, 11 cho `Tracker` và
`InteractionManager`. Property 9 và 12 nằm trong frame loop của `src/main.cpp`. Không property nào cần
NPU. Trong dòng **Validates**, `X.Y` là mục `RX.Y` trong requirements.md.

### Property 1: Fingerprint chỉ phụ thuộc nội dung runtime

*Với mọi* snapshot catalog, fingerprint không đổi khi đổi thứ tự hàng trả về hoặc khi chỉ
`match_events`, `last_seen_at`, `match_count` thay đổi. Đổi bất kỳ trường runtime nào của resident
(`id`, `name`, `apartment`, `home_floor`, `language`, `greeting_name`, `role`, `ext_id`), thêm/xóa
embedding, hoặc đổi `source`/kích thước/byte vector làm fingerprint khác (trừ va chạm hash).

**Validates: Requirements 2.5, 2.6, 3.3**

### Property 2: Diff đúng theo resident

*Với mọi* cặp catalog cũ/mới, `CatalogDiff.added` là các resident ID chỉ có trong catalog mới,
`removed` là các ID chỉ có trong catalog cũ (gồm resident bị deactivate/delete), `changed` là các ID
có ở cả hai nhưng `ResidentDigest` khác. Resident có digest không đổi không thuộc tập nào.

**Validates: Requirements 2.6, 5.2, 5.3**

### Property 3: Không rebuild khi catalog không đổi

*Với mọi* chuỗi poll, nếu `data_version` bằng accepted version và không có force/pending thì `poll()`
trả `NoDbChange` và không load snapshot. Nếu snapshot có fingerprint bằng active fingerprint thì
`poll()` trả `NoCatalogChange`; generation, matcher và resident map không đổi. Poll chỉ chạy khi tới
deadline theo monotonic clock, không chạy theo từng frame.

**Validates: Requirements 1.3, 2.3, 2.5, 7.1, 7.3**

### Property 4: Snapshot nhất quán

*Với mọi* thời điểm writer commit, kể cả giữa hai SELECT, residents và embeddings của candidate thuộc
cùng một commit point. Không embedding nào của candidate trỏ tới resident không có trong candidate.

**Validates: Requirements 3.2**

### Property 5: Enroll atomic

*Với mọi* batch `add_person` (`--replace` hoặc `--merge`, nhiều ảnh) và mọi điểm lỗi giữa
`BEGIN IMMEDIATE` và COMMIT, DB sau đó hoặc giữ nguyên toàn bộ trạng thái cũ hoặc chứa toàn bộ batch
mới. JSON `ok:true` chỉ xuất hiện khi COMMIT thành công. Reader không bao giờ thấy resident của batch
với zero/partial embeddings.

**Validates: Requirements 3.1, 8.4**

### Property 6: Candidate được chấp nhận hoặc từ chối nguyên khối

*Với mọi* candidate có ít nhất một embedding vi phạm mục "Điều kiện hợp lệ của candidate" (NULL blob,
dim khác `recog_dim`, `blob_nbytes != declared_dim*sizeof(float)`, NaN/Inf, norm ≤ epsilon,
`abs(norm - 1.0) > 0.05`) hoặc build ra số vector khác số embedding, `poll()` trả `Failed`; active
matcher, resident map và generation không đổi. Catalog rỗng `0/0` là candidate hợp lệ.

**Validates: Requirements 3.5, 3.8, 4.1, 4.2, 4.3, 4.4, 4.9**

### Property 7: Retry sau lỗi

*Với mọi* reload thất bại (busy, SELECT lỗi, candidate không hợp lệ), accepted data version không
được advance và `pending_retry_` bật. Chu kỳ sau load snapshot lại dù `data_version` không tăng, cho
tới khi reload thành công hoặc fingerprint bằng active.

**Validates: Requirements 3.6**

### Property 8: Reconnect force full snapshot

*Với mọi* lần reader mở lại, accepted data version bị xóa và poll đầu tiên sau đó load snapshot bất kể
giá trị `data_version` mới, kể cả khi trùng số cũ. Khi reader lỗi, open được thử lại với backoff 1–10 s
và active catalog vẫn phục vụ.

**Validates: Requirements 3.7**

### Property 9: Một frame dùng đúng một generation

*Với mọi* frame, matcher và `resident_by_id` được dùng thuộc cùng một generation. Swap chỉ xảy ra trên
main thread tại frame boundary, trong cùng block với reconcile cache. Mỗi reload thành công tăng
generation đúng 1.

**Validates: Requirements 4.5, 4.6, 4.7, 5.8**

### Property 10: Invalidate tracker có chọn lọc

*Với mọi* `CatalogDiff` và trạng thái tracker, một track bị invalidate khi và chỉ khi nó map tới
resident trong `changed ∪ removed`, hoặc nó unknown/unrecognized và `added` không rỗng. Track bị
invalidate có `name` rỗng, `match_sim = -1`, `last_recog_frame = -1` và `needs_recog()` true ở frame kế;
mapping của nó bị xóa khỏi `track_resident_id`. Mọi track giữ nguyên bbox, `id`, state và hit/miss
counters. Sau mỗi content reload, `ghosts_` rỗng.

**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

### Property 11: Drop session giữ cooldown

*Với mọi* tập subject key, `drop_sessions()` chỉ xóa session của các key đó; `last_confirmed_ms_` và
session của key khác không đổi. Ở SCRFD-only mode, key 0 chỉ bị drop khi `SessionView.resident_id` thuộc
`changed ∪ removed`. Resident không thuộc diff không nhận thêm confirmed event hay floor action chỉ do
reload.

**Validates: Requirements 5.6, 5.7**

### Property 12: Nhịp poll độc lập frame

*Với mọi* mẫu frame đến, kể cả không có frame nào, poll chạy khi tới reload deadline; khoảng giữa hai
poll không nhỏ hơn `resident_reload_ms` và frame loop không busy-loop. Với `--resident-reload-ms 0`,
reader không được mở và wait vô hạn như trước.

**Validates: Requirements 1.2, 1.4, 7.5, 8.3**

### Property 13: Log không chứa PII

*Với mọi* log `[db-reload]` và log tracker liên quan, nội dung chỉ gồm interval, generation, số
residents/embeddings, latency, số affected tracks, loại lỗi và ID số; không có `name`, `greeting_name`,
apartment, vector hay đường dẫn ảnh.

**Validates: Requirements 5.9, 6.4** (và hard rule về PII trong `AGENTS.md`)

## Error Handling

Nguyên tắc: reload lỗi không được crash app, clear matcher hay chặn frame loop lâu hơn busy timeout
của reader. Khi không tin được candidate, app giữ generation đang chạy và retry ở chu kỳ sau. Lỗi trả
qua `bool`/`PollResult` kèm log, không throw; log chỉ chứa loại lỗi và ID số.

| Tình huống | Hành vi |
|---|---|
| Chỉ match_event/touch commit | data_version đổi, fingerprint bằng → no swap |
| add_person replace nhiều ảnh | một BEGIN/COMMIT; reader thấy toàn batch cũ hoặc mới, không zero/partial |
| Camera không có frame | timed wait thức theo deadline; poll vẫn chạy, không NPU |
| SQLITE_BUSY/read error | timeout ngắn, giữ generation cũ, WARN rate-limited, retry |
| Reader reconnect | reset accepted version, force full snapshot trước fast path |
| NULL/trailing bytes/dim sai/NaN/Inf/zero/norm lỗi | từ chối toàn candidate, giữ matcher cũ |
| Resident mới không embedding | metadata reload; matcher không có vector cho người đó |
| Catalog rỗng | swap matcher rỗng, WARN everyone unknown |
| Reader connection chết | retry read-only open có backoff 1–10s; active matcher vẫn chạy |
| Catalog đổi khi có ghost | purge ghosts để không inherit stale name/identity |
| App shutdown lúc poll | hoàn tất/abort read, close reader, teardown bình thường |

## Testing Strategy

Test chạy bằng `bash tests/run_tests.sh`; mỗi test là một `main()` riêng và phải được đăng ký trong
script. Số trong ngoặc là property mà test kiểm.

### Unit tests không NPU

1. `data_version`: connection B commit → reader thấy đổi; cùng reader read không tự tăng (Property 3).
2. Snapshot consistency dùng test-only barrier: writer commit chính xác giữa hai SELECT nhưng candidate
   vẫn không orphan/partial; test không phụ thuộc sleep race (Property 4).
3. `add_person` transaction: replace nhiều embeddings commit all-or-nothing; inject write fail rollback
   resident metadata + old embeddings nguyên vẹn; JSON ok:true chỉ sau commit (Property 5).
4. Fingerprint stable khi row order khác; đổi runtime metadata/vector → hash đổi (Property 1).
5. `match_events` và `last_seen_at/match_count` → fingerprint không đổi (Property 1, 3).
6. Add/replace/delete/deactivate → diff đúng (Property 2).
7. Blob invalid: declared dim mismatch, NULL, trailing bytes, NaN, Inf, zero và norm ngoài tolerance →
   candidate rejected, active generation/matcher giữ nguyên (Property 6).
8. Tracker invalidation: geometry/ID giữ, `needs_recog` true; catalog change purge ghosts; không log PII
   (Property 10, 13).
9. Interaction drop affected session giữ cooldown; subject khác giữ session (Property 11).
10. Reader busy trả trong timeout ngắn; reconnect force full snapshot dù data_version số trùng
    (Property 8).
11. Không-frame timed wait vẫn poll theo cadence (Property 12).
12. `--resident-reload-ms 0/1000/invalid` parser behavior (Property 12, phần cấu hình).

### Integration SQLite không NPU

- App-like read-only reader + writer connection; atomic `add_person`-equivalent batch transaction
  (Property 5).
- Poll trước COMMIT → old generation; sau COMMIT → đúng một reload với toàn bộ embeddings
  (Property 5, 9).
- Audit writer commits nhiều lần → zero content reload (Property 3).
- Failure then retry with same data_version → reload được sau khi lỗi hết (Property 7).
- Reader close/reopen → force snapshot dù số data_version mới tình cờ bằng số cũ (Property 8).
- Giữ capture slot không publish frame → timed wake vẫn poll/reload theo interval, không busy-loop
  (Property 12).

### Smoke NPU có kiểm soát

- Chỉ một lần, không dùng `timeout 6` trần; dùng `timeout -k 3s ...` để không để process sót.
- Start face app, enroll ảnh thật qua web/add_person khi PID không đổi.
- Quan sát NPU contention ngắn được chấp nhận.
- Xác nhận log generation tăng và resident mới match mà không restart.
- Benchmark trước/sau khi catalog không đổi để phát hiện regression FPS.

## Hiệu năng

Fast path mỗi 1000 ms: một `PRAGMA data_version`, không query theo frame. Khi app ghi audit, một
snapshot/hash có thể chạy nhưng fingerprint bằng active → không build/swap. Catalog hiện vài hàng;
1000 residents × 512 float khoảng 2 MiB, vẫn nhỏ so với video frame traffic.

Metrics/log đề xuất:

```text
[db-reload] enabled interval_ms=1000
[db-reload] generation=2 residents=4 embeddings=5 latency_ms=... affected_tracks=1
[db-reload] candidate rejected reason=dim_mismatch retry_in_ms=1000
```

Không log resident names/greeting/apartment/path.

## Rollout và rollback

- Mặc định bật 1000 ms cho `--resident-db`.
- Có thể rollback runtime bằng `--resident-reload-ms 0` mà không đổi DB/schema.
- Lần deploy đầu phải restart app để chạy binary mới; từ lần enroll sau không cần restart.
- UDS đợt b có thể thay trigger/pipeline enroll sau này nhưng `ResidentCatalog` reload/swap vẫn tái sử
  dụng được cho sync cloud hoặc admin DB changes.
