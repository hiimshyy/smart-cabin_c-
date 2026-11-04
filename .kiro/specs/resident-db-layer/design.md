# Design — Lớp SQLite + Multi-embedding + State Machine tương tác

**Spec ID**: `resident-db-layer`

Tài liệu yêu cầu: [requirements.md](requirements.md). Kế hoạch triển khai: [tasks.md](tasks.md).

## Overview

Thiết kế này thay lớp dữ liệu `.fdb` (1 embedding trung bình/người, không có tầng/ngôn ngữ/audit) bằng SQLite theo `db/schema.sql`, thêm matching đa embedding và một state machine gom các frame thành một phiên tương tác. Phần detect → align → recog giữ nguyên; thay đổi bắt đầu từ bước match embedding.

Các quyết định chính:

- `ResidentDB` mở SQLite ở chế độ WAL, đọc schema từ `db/schema.sql` lúc runtime, và ghi `match_events` qua writer thread nền để luồng frame không chờ disk I/O.
- `MatchEngine` giữ mọi embedding (không gộp trung bình) trong một mảng phẳng liên tục, match bằng duyệt tuyến tính dot-product và lấy similarity lớn nhất.
- `InteractionManager` chỉ phát sự kiện khi một subject match cùng resident `confirm_streak` frame liên tiếp, và áp cooldown theo resident để chống flip-flop.
- `migrate_fdb` chuyển `.fdb` cũ sang SQLite; resident chưa có thông tin tầng mang sentinel `home_floor = 0`.
- Các module mới **tách khỏi NPU** để build/test được trên máy dev (chỉ cần OpenCV + sqlite3). `main.cpp` là nơi khâu nối.

### Phạm vi theo giai đoạn

- **Giai đoạn 1 (R1–R5):** `ResidentDB`, `MatchEngine`, `InteractionManager`, `migrate_fdb` và khâu nối `main.cpp` dùng SQLite.
- **Giai đoạn 2 (R6):** `enroll_faces` và `add_person` ghi thẳng SQLite. Sau giai đoạn này `.fdb` chỉ còn là input cho `migrate_fdb` và chế độ dev `--face-db`.

## Architecture

```
                         ┌──────────────────────────┐
   frame → detect → align→ recog (embedding 512-D)  │  (giữ nguyên)
                         └─────────────┬────────────┘
                                       ▼
                          ┌─────────────────────────┐
                          │  MatchEngine            │  R2: multi-embedding
                          │  (in-memory vectors)    │  max cosine → resident_id
                          └─────────────┬───────────┘
                                        ▼
                          ┌─────────────────────────┐
                          │  InteractionManager     │  R4: state machine
                          │  IDLE→DETECTING→MATCHED │  + cooldown
                          │       →CONFIRMED        │
                          └─────────────┬───────────┘
                                        ▼
                          ┌─────────────────────────┐
                          │  ResidentDB (SQLite/WAL)│  R1: CRUD + events
                          │  residents / embeddings │  async event writer
                          │  match_events           │
                          └─────────────────────────┘
```

### Module & file mới

| File                        | Vai trò                                                                                  |
| --------------------------- | ----------------------------------------------------------------------------------------- |
| `src/resident_db.h/.cpp`  | Wrapper SQLite (R1). Mở WAL, apply schema, đọc residents+embeddings, ghi events async. |
| `src/match_engine.h/.cpp` | Multi-embedding matching (R2). Giữ vector phẳng trong RAM, cosine max.                  |
| `src/interaction.h/.cpp`  | State machine phiên tương tác (R4).                                                   |
| `src/migrate_fdb.cpp`     | Tool CLI migrate `.fdb` → SQLite (R3).                                                 |
| `db/schema.sql`           | (đã có) nguồn schema; nhúng vào binary hoặc đọc runtime.                         |

Sửa: `src/main.cpp` (khâu nối, cờ CLI mới), `Makefile` (thêm sqlite3, target `migrate_fdb`).

### Khâu nối trong `main.cpp` (R5 — Giai đoạn 1)

- Thêm cờ: `--resident-db <path>`, `--cabin-id N`, `--confirm-streak N`, `--cooldown-ms N`.
- Khi có `--resident-db`: khởi tạo `ResidentDB.open()` → `load_active()` → `MatchEngine.build()`. Trong vòng lặp, dùng `match_engine.match()`, feed kết quả vào `InteractionManager.update()`; với mỗi Outcome, gọi `resident_db.log_event()` (+ `touch_resident` nếu matched). Overlay hiển thị `greeting_name`/floor lấy từ map resident_id→Resident.
- Đường `.fdb` cũ (`--face-db`) giữ lại như chế độ test/dev đơn giản (không tầng/ngôn ngữ/audit); usage ghi rõ "không dùng cho cabin thật". Không xoá code `.fdb` vì `migrate_fdb` vẫn cần `FaceDB::load()`.
- Cleanup: `resident_db.close()` (flush writer) trước khi thoát.

### Thay đổi build (Makefile)

- Thêm `-lsqlite3` vào `LIBS`; đảm bảo `libsqlite3-dev` cài trên Orange Pi.
- Thêm `resident_db.cpp`, `match_engine.cpp`, `interaction.cpp` vào `APP_SRCS_CPP` (Giai đoạn 1).
- Target mới `migrate_fdb` (chỉ cần `face_db.cpp` + `resident_db.cpp` + sqlite, KHÔNG cần SDK NPU) → build/test trên máy dev.
- Giai đoạn 2: `ENROLL_SRCS_CPP` và `ADD_SRCS_CPP` thêm `resident_db.cpp` + link `-lsqlite3`.

## Components and Interfaces

### `ResidentDB` (R1)

```cpp
class ResidentDB {
public:
    bool open(const std::string& path);      // WAL + foreign_keys + apply schema nếu trống
    // Nạp toàn bộ để feed MatchEngine
    bool load_active(std::vector<Resident>& residents,
                     std::vector<EmbeddingRow>& embeddings);
    // Ghi event (đẩy vào queue, ghi ở thread nền)
    void log_event(const MatchEvent& ev);
    // Cập nhật thống kê khi match
    void touch_resident(int64_t resident_id);
    void close();                            // flush queue + join writer thread
private:
    sqlite3* db_ = nullptr;
    // async writer: queue + mutex + cv + thread
};
```

**Chi tiết quyết định:**

- **Apply schema**: đọc `db/schema.sql` runtime (đường dẫn cấu hình được) HOẶC nhúng chuỗi. Chọn đọc runtime cho v1 để giữ 1 nguồn schema; fallback lỗi rõ ràng nếu thiếu file.
- **Async event writer**: `log_event` chỉ push vào queue trong bộ nhớ + notify; 1 thread nền mở transaction batch mỗi ~500ms hoặc mỗi N event. Đảm bảo luồng frame không bị chặn bởi I/O disk (R-NFR).
- **touch_resident**: gộp vào batch cùng writer thread (UPDATE last_seen_at, match_count).
- Prepared statements tái dùng; `sqlite3_bind_blob` cho vector.

### `MatchEngine` (R2)

```cpp
class MatchEngine {
public:
    void build(const std::vector<EmbeddingRow>& rows, int dim);
    MatchResult match(const std::vector<float>& query, float threshold) const;
    size_t vector_count() const;
private:
    int                 dim_ = 512;
    std::vector<float>  flat_;        // count*dim, contiguous → cache-friendly
    std::vector<int64_t> owner_;      // count, resident_id của mỗi vector
};
```

**Quyết định:**

- Lưu **contiguous flat array** (không phải `vector<vector<float>>`) để cosine chạy nhanh, cache-friendly. Với 10000×512 float = ~20MB RAM, chấp nhận được.
- `match` = duyệt tuyến tính dot-product (vector đã L2-norm). Ước lượng ~5.1M phép nhân → dưới ~2ms trên A733. Nếu sau này chậm, nâng cấp SIMD/ANN ở version sau (ngoài scope v1).
- Trả `resident_id` của vector có similarity cao nhất vượt ngưỡng.
- Tái dùng `cosine_sim_normalized` từ `face_db.cpp` (hoặc bản inline tương đương).

### `InteractionManager` (R4)

Quản lý phiên tương tác. Đầu vào mỗi frame: danh sách (subject_key, MatchResult) — `subject_key` là `track_id` nếu có tracker, ngược lại là index face ổn định nhất (largest face).

```cpp
class InteractionManager {
public:
    // Trả về danh sách sự kiện "vừa CONFIRMED" / "vừa unknown" để main ghi DB.
    struct Outcome { int64_t resident_id; float sim; bool confirmed; bool unknown; };
    std::vector<Outcome> update(
        const std::vector<std::pair<int,MatchResult>>& subjects, double now_ms);
private:
    std::map<int, Session> sessions_;
    std::map<int64_t, double> last_confirmed_ms_;  // resident_id → thời điểm confirm
    InteractionConfig cfg_;
};
```

**Logic chuyển trạng thái:**

- Subject mới xuất hiện → tạo Session (DETECTING).
- Match cùng resident liên tiếp `confirm_streak` frame → CONFIRMED (nếu ngoài cooldown của resident đó) → phát Outcome{confirmed}.
- CONFIRMED xong ghi `last_confirmed_ms_[resident_id]`; trong `cooldown_ms` không phát confirmed lại.
- Subject có mặt > `unknown_after_ms` mà chưa match → phát Outcome{unknown} một lần.
- Subject biến mất (không còn trong `subjects`) → xóa Session.

### `migrate_fdb` (R3)

Luồng: `FaceDB::load(fdb)` → với mỗi `Identity`: `INSERT residents(name,...defaults)` → lấy `resident_id` → `INSERT embeddings(resident_id, source='id_photo', vector=blob)`. Kiểm tra tồn tại theo name (mặc định skip, `--overwrite` xóa cũ). Dùng `ResidentDB`/sqlite trực tiếp; không cần NPU nên build được trên máy dev.

CLI: `./migrate_fdb --fdb db/faces_all.fdb --db db/residents.db [--overwrite]`.
Lưu ý: `home_floor` NOT NULL trong schema → migrate đặt **`home_floor = 0`** (sentinel "chưa đăng ký tầng", xem requirements §6) cho mọi resident + log cảnh báo tổng số resident chưa có tầng để HR cập nhật sau. Không tự đặt tầng thật (ví dụ 1) vì sẽ khiến cabin auto-gọi sai tầng.

### Chuyển enroll tools sang SQLite (R6 — Giai đoạn 2)

- **`enroll_faces`**: thay `FaceDB db; db.add(); db.save(fdb)` bằng ghi SQLite qua `ResidentDB`. Mỗi ảnh → 1 hàng `embeddings(source='id_photo')` (KHÔNG trung bình). Tạo `residents(name, home_floor=0)` nếu chưa tồn tại (match theo name). CLI đổi `--out <fdb>` → `--db <sqlite>`.
- **`add_person`**: thay đọc/ghi `.fdb` bằng thao tác SQLite. `--merge` = INSERT thêm embedding cho resident. `--replace` = DELETE embeddings cũ của resident rồi INSERT mới. Tạo resident nếu chưa có. CLI đổi `--db <fdb>` → `--db <sqlite>`.
- Bổ sung API vào `ResidentDB`: `upsert_resident(name, ...) -> id`, `add_embedding(resident_id, source, vector)`, `delete_embeddings(resident_id)`.
- `capture_person` không đổi.

## Data Models

Schema bảng (`residents`, `embeddings`, `cabins`, `match_events`) lấy từ `db/schema.sql`, là nguồn chân lý schema; spec này không đổi schema. Các struct dưới đây là ánh xạ in-memory.

### Bản ghi DB

```cpp
struct Resident {
    int64_t     id;
    std::string name;
    std::string apartment;
    int         home_floor;
    std::string language;       // "vi" | "en"
    std::string greeting_name;
    std::string role;
};

struct EmbeddingRow {
    int64_t            resident_id;
    std::string        source;   // id_photo | cabin | admin
    std::vector<float> vector;   // L2-normalized, dim floats
};

struct MatchEvent {
    int         cabin_id;
    int64_t     resident_id;     // <0 => NULL (unknown)
    float       similarity;
    float       liveness_score;  // -1 => NULL ở v1
    std::string action;          // matched|unknown|cancelled|...
    int         floor_selected;  // 0 => none
    int         latency_ms;
};
```

`EmbeddingRow.vector` lưu xuống cột BLOB của `embeddings` qua `sqlite3_bind_blob`.

### Kết quả match

```cpp
struct MatchResult {
    int64_t resident_id = -1;   // -1 => unknown
    float   similarity  = -1.f;
    int     emb_index   = -1;   // vector nào thắng (debug/self-supervised sau)
};
```

### Phiên tương tác

```cpp
enum class SessionState { IDLE, DETECTING, MATCHED, CONFIRMED };

struct Session {
    int          subject_key;
    SessionState state = SessionState::DETECTING;
    int64_t      resident_id = -1;
    float        best_sim = -1.f;
    int          matched_streak = 0;   // số frame liên tiếp match cùng người
    double       first_seen_ms = 0;
    double       confirmed_ms  = 0;
};

struct InteractionConfig {
    int    confirm_streak   = 5;      // frame liên tiếp match để CONFIRMED
    double cooldown_ms      = 3000;   // chống flip-flop cùng người
    double unknown_after_ms = 2000;   // có mặt nhưng không match → log unknown
};
```

### Quy ước giá trị sentinel

| Trường | Giá trị | Ý nghĩa |
| --- | --- | --- |
| `Resident.home_floor` | `<= 0` (ghi `0`) | Chưa đăng ký tầng (requirements §6). Cabin vẫn chào tên nhưng không auto-gọi tầng. |
| `MatchEvent.resident_id` | `< 0` | Ghi `NULL` (unknown). |
| `MatchEvent.liveness_score` | `-1` | Ghi `NULL` ở v1. |
| `MatchEvent.floor_selected` | `0` | Không chọn tầng / cancelled. |
| `MatchResult.resident_id` | `-1` | Unknown (dưới ngưỡng). |

Ở v1, `match_events.snapshot` để trống và `embeddings.quality_score` chưa tính (requirements §5).

## Correctness Properties

*Property là đặc tính phải đúng trên mọi lần chạy hợp lệ của hệ thống, tức một phát biểu hình thức về việc hệ thống phải làm gì. Property là cầu nối giữa đặc tả đọc được và kiểm chứng tự động.*

Trong dòng **Validates**, `RX.Y` là yêu cầu `RX` ý thứ `Y` ở mục 3 của [requirements.md](requirements.md); `§N` là mục `N` của requirements.

### Property 1: Mỗi embedding giữ riêng một vector

*Với mọi* tập `EmbeddingRow` có đúng `dim` phần tử, `MatchEngine::build` giữ đúng một vector cho mỗi row (`vector_count() == rows.size()`) và `owner_` của vector đó là `resident_id` của row. Không có bước gộp trung bình nào, kể cả khi `enroll_faces` ghi nhiều ảnh của cùng một người.

**Validates: Requirements 2.1, 6.1**

### Property 2: Match theo similarity lớn nhất và ngưỡng

*Với mọi* query đã L2-normalize, `match` tính dot-product với tất cả vector. Nếu similarity lớn nhất `>= threshold`, kết quả là `resident_id` và `similarity` của vector đó; ngược lại `resident_id = -1` (unknown).

**Validates: Requirements 2.2, 2.3, 2.5**

### Property 3: CONFIRMED cần đủ streak

*Với mọi* chuỗi frame, một session chuyển sang CONFIRMED cho resident `R` khi và chỉ khi subject match `R` liên tiếp `confirm_streak` frame và `R` nằm ngoài cooldown. Mỗi lần chuyển sang CONFIRMED phát đúng một `Outcome{confirmed}`, tương ứng đúng một hàng `match_events` với `action='matched'`.

**Validates: Requirements 4.1, 4.3**

### Property 4: Cooldown theo resident

*Với mọi* resident `R` được confirm tại thời điểm `t0`, không có `Outcome{confirmed}` nào cho `R` trong khoảng `cooldown_ms` sau `t0`, kể cả từ một `subject_key` khác, vì `last_confirmed_ms_` khóa theo `resident_id`.

**Validates: Requirements 4.2**

### Property 5: Unknown phát một lần

*Với mọi* subject có mặt lâu hơn `unknown_after_ms` mà chưa match, session phát đúng một `Outcome{unknown}`, tương ứng một hàng `match_events` với `action='unknown'`.

**Validates: Requirements 4.4**

### Property 6: Vòng đời session theo subject

*Với mọi* frame, subject không còn trong `subjects` bị xóa session; subject xuất hiện lại tạo session mới ở DETECTING. `subject_key` là `track_id` khi có tracker, ngược lại là index của face lớn nhất, nên cùng logic chạy được cho cả hai pipeline.

**Validates: Requirements 4.1, 4.5**

### Property 7: Mở DB và nạp dữ liệu

*Với mọi* đường dẫn hợp lệ, `open` bật WAL và `foreign_keys=ON`; nếu DB chưa có bảng thì áp dụng `db/schema.sql`. `load_active` chỉ trả residents `active=1` cùng toàn bộ embeddings của họ.

**Validates: Requirements 1.1, 1.2, 1.3**

### Property 8: Event và thống kê được ghi đủ

*Với mọi* chuỗi `log_event`/`touch_resident` trước `close()`, sau khi writer flush: mỗi `log_event` tạo đúng một hàng `match_events`, mỗi `touch_resident` tăng `match_count` đúng 1 và cập nhật `last_seen_at`.

**Validates: Requirements 1.4, 1.5**

### Property 9: Luồng frame không làm SQLite I/O

*Với mọi* lời gọi `log_event`/`touch_resident` từ luồng frame, lời gọi chỉ push vào queue trong bộ nhớ và notify. Mọi câu lệnh SQLite ghi chạy trên writer thread theo batch transaction. `close()` flush queue rồi mới join writer thread.

**Validates: Requirements 4**

### Property 10: Migrate đếm đúng và chạy lại an toàn

*Với mọi* `.fdb` có `N` identity, chạy `migrate_fdb` vào DB trống tạo `N` residents và `N` embeddings `source='id_photo'`. Chạy lại không có `--overwrite` bỏ qua mọi identity trùng tên và không đổi số hàng; có `--overwrite` thì thay bản cũ, không nhân đôi. Tool luôn in số identity đã import và số bỏ qua.

**Validates: Requirements 3.1, 3.2, 3.3, 3.4**

### Property 11: Sentinel `home_floor`

*Với mọi* resident do `migrate_fdb` hoặc `enroll_faces` tạo khi chưa có thông tin tầng, `home_floor = 0`; không tool nào tự gán tầng thật. `migrate_fdb` in cảnh báo tổng số resident chưa có tầng.

**Validates: Requirements 6.1, 6**

### Property 12: `add_person` merge/replace

*Với mọi* resident có `k` embedding và lần chạy `add_person` với `m` ảnh mới: `--merge` để lại `k + m` embedding; `--replace` để lại đúng `m` embedding mới. Resident chưa có thì được tạo trước.

**Validates: Requirements 6.2**

### Property 13: Lỗi SQLite không crash

*Với mọi* câu lệnh SQLite lỗi, `ResidentDB` trả `false`/mã lỗi rõ ràng và tiếp tục chạy, không crash.

**Validates: Requirements 1.6**

## Error Handling

Lỗi đi theo convention của repo: trả `bool` + log, không throw.

| Tình huống | Xử lý |
| --- | --- |
| Thiếu file `db/schema.sql` khi cần apply schema | `open` báo lỗi rõ ràng (đã chọn đọc schema runtime, không nhúng chuỗi). |
| Câu lệnh SQLite lỗi (prepare/bind/step) | Trả `false`/mã lỗi rõ ràng, log, không crash (R1.6). |
| Lệch dim giữa embedding và `MatchEngine` (512) | Kiểm tra dim khi `build`, báo lỗi rõ ràng. |
| Max similarity dưới ngưỡng | `match` trả `resident_id = -1` (unknown) (R2.3). |
| Có mặt quá `unknown_after_ms` mà chưa match | Phát `Outcome{unknown}` một lần, ghi `action='unknown'` (R4.4). |
| Resident cùng tên đã tồn tại khi migrate | Mặc định bỏ qua + log; `--overwrite` xóa bản cũ rồi ghi lại (R3.3). |
| `.fdb` không có thông tin tầng | Đặt `home_floor = 0` và in tổng số resident chưa có tầng (§6). |
| SQLite I/O chậm | Luồng frame chỉ push queue; writer thread ghi batch transaction. |
| Shutdown | `resident_db.close()` flush queue và join writer thread trước khi thoát. |

## Testing Strategy

Số trong ngoặc là property mà test kiểm.

- **Unit (máy dev, không NPU):**
  - `ResidentDB`: open tạo schema; insert resident+embedding; load_active trả đúng (Property 7); log_event ghi được; touch_resident tăng count (Property 8).
  - `MatchEngine`: build từ vector giả; match trả đúng resident có max cosine; dưới ngưỡng → unknown (Property 1, 2).
  - `InteractionManager`: chuỗi frame giả → CONFIRMED sau đúng streak (Property 3); cooldown chặn lần 2 (Property 4); unknown sau timeout (Property 5).
  - `migrate_fdb`: fdb mẫu → SQLite có đúng số residents+embeddings; chạy lại skip/overwrite đúng (Property 10, 11).
- **Tích hợp (Orange Pi):** chạy `main` với `--resident-db`, đối chiếu `match_events` sau vài lần đi qua camera; xác nhận `.fdb` mode vẫn chạy.
- **Không hồi quy:** build + chạy `enroll_faces`/`add_person` như cũ.

Danh sách trên chưa nêu test riêng cho Property 6, 9, 12 và 13.

## Rủi ro

| Rủi ro                                  | Giảm thiểu                                                           |
| ---------------------------------------- | ---------------------------------------------------------------------- |
| SQLite I/O làm sụt FPS                 | Async writer thread + batch transaction; luồng frame chỉ push queue. |
| Match tuyến tính chậm khi >10k vector | Flat array cache-friendly; đo thực tế; ANN để version sau.        |
| `home_floor` NOT NULL khi migrate      | Đặt sentinel `0` = "chưa đăng ký tầng" (req §6) + log tổng số để HR sửa. Cabin coi `<=0` là không auto-gọi tầng. |
| Lệch dim giữa fdb và schema (512)     | Kiểm tra dim khi build MatchEngine, lỗi rõ ràng.                   |
