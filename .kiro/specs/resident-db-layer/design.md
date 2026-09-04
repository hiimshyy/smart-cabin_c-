# Design — Lớp SQLite + Multi-embedding + State Machine tương tác

**Spec ID**: `resident-db-layer`

## 1. Kiến trúc tổng thể

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

Các module mới đều **tách khỏi NPU** để build/test được trên máy dev (chỉ cần OpenCV + sqlite3). `main.cpp` là nơi khâu nối.

## 2. Module & file mới

| File                        | Vai trò                                                                                  |
| --------------------------- | ----------------------------------------------------------------------------------------- |
| `src/resident_db.h/.cpp`  | Wrapper SQLite (R1). Mở WAL, apply schema, đọc residents+embeddings, ghi events async. |
| `src/match_engine.h/.cpp` | Multi-embedding matching (R2). Giữ vector phẳng trong RAM, cosine max.                  |
| `src/interaction.h/.cpp`  | State machine phiên tương tác (R4).                                                   |
| `src/migrate_fdb.cpp`     | Tool CLI migrate`.fdb`→SQLite (R3).                                                    |
| `db/schema.sql`           | (đã có) nguồn schema; nhúng vào binary hoặc đọc runtime.                         |

Sửa: `src/main.cpp` (khâu nối, cờ CLI mới), `Makefile` (thêm sqlite3, target `migrate_fdb`).

## 3. `ResidentDB` (R1)

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

## 4. `MatchEngine` (R2)

```cpp
struct MatchResult {
    int64_t resident_id = -1;   // -1 => unknown
    float   similarity  = -1.f;
    int     emb_index   = -1;   // vector nào thắng (debug/self-supervised sau)
};

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

- Lưu **contiguous flat array** (không phải vector<vector></vector>) để cosine chạy nhanh, cache-friendly. Với 10000×512 float = ~20MB RAM, chấp nhận được.
- `match` = duyệt tuyến tính dot-product (vector đã L2-norm). Ước lượng ~5.1M phép nhân → dưới ~2ms trên A733. Nếu sau này chậm, nâng cấp SIMD/ANN ở version sau (ngoài scope v1).
- Trả `resident_id` của vector có similarity cao nhất vượt ngưỡng.
- Tái dùng `cosine_sim_normalized` từ `face_db.cpp` (hoặc bản inline tương đương).

## 5. `InteractionManager` (R4)

Quản lý phiên tương tác. Đầu vào mỗi frame: danh sách (subject_key, MatchResult) — `subject_key` là `track_id` nếu có tracker, ngược lại là index face ổn định nhất (largest face).

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

## 6. `migrate_fdb` (R3)

Luồng: `FaceDB::load(fdb)` → với mỗi `Identity`: `INSERT residents(name,...defaults)` → lấy `resident_id` → `INSERT embeddings(resident_id, source='id_photo', vector=blob)`. Kiểm tra tồn tại theo name (mặc định skip, `--overwrite` xóa cũ). Dùng `ResidentDB`/sqlite trực tiếp; không cần NPU nên build được trên máy dev.

CLI: `./migrate_fdb --fdb db/faces_all.fdb --db db/residents.db [--overwrite]`.
Lưu ý: `home_floor` NOT NULL trong schema → migrate đặt **`home_floor = 0`** (sentinel "chưa đăng ký tầng", xem requirements §6) cho mọi resident + log cảnh báo tổng số resident chưa có tầng để HR cập nhật sau. Không tự đặt tầng thật (ví dụ 1) vì sẽ khiến cabin auto-gọi sai tầng.

## 7. Khâu nối trong `main.cpp` (R5 — Giai đoạn 1)

- Thêm cờ: `--resident-db <path>`, `--cabin-id N`, `--confirm-streak N`, `--cooldown-ms N`.
- Khi có `--resident-db`: khởi tạo `ResidentDB.open()` → `load_active()` → `MatchEngine.build()`. Trong vòng lặp, dùng `match_engine.match()`, feed kết quả vào `InteractionManager.update()`; với mỗi Outcome, gọi `resident_db.log_event()` (+ `touch_resident` nếu matched). Overlay hiển thị `greeting_name`/floor lấy từ map resident_id→Resident.
- Đường `.fdb` cũ (`--face-db`) giữ lại như chế độ test/dev đơn giản (không tầng/ngôn ngữ/audit); usage ghi rõ "không dùng cho cabin thật". Không xoá code `.fdb` vì `migrate_fdb` vẫn cần `FaceDB::load()`.
- Cleanup: `resident_db.close()` (flush writer) trước khi thoát.

## 7b. Chuyển enroll tools sang SQLite (R6 — Giai đoạn 2)

- **`enroll_faces`**: thay `FaceDB db; db.add(); db.save(fdb)` bằng ghi SQLite qua `ResidentDB`. Mỗi ảnh → 1 hàng `embeddings(source='id_photo')` (KHÔNG trung bình). Tạo `residents(name, home_floor=0)` nếu chưa tồn tại (match theo name). CLI đổi `--out <fdb>` → `--db <sqlite>`.
- **`add_person`**: thay đọc/ghi `.fdb` bằng thao tác SQLite. `--merge` = INSERT thêm embedding cho resident. `--replace` = DELETE embeddings cũ của resident rồi INSERT mới. Tạo resident nếu chưa có. CLI đổi `--db <fdb>` → `--db <sqlite>`.
- Bổ sung API vào `ResidentDB`: `upsert_resident(name, ...) -> id`, `add_embedding(resident_id, source, vector)`, `delete_embeddings(resident_id)`.
- `capture_person` không đổi.

## 8. Thay đổi build (Makefile)

- Thêm `-lsqlite3` vào `LIBS`; đảm bảo `libsqlite3-dev` cài trên Orange Pi.
- Thêm `resident_db.cpp`, `match_engine.cpp`, `interaction.cpp` vào `APP_SRCS_CPP` (Giai đoạn 1).
- Target mới `migrate_fdb` (chỉ cần `face_db.cpp` + `resident_db.cpp` + sqlite, KHÔNG cần SDK NPU) → build/test trên máy dev.
- Giai đoạn 2: `ENROLL_SRCS_CPP` và `ADD_SRCS_CPP` thêm `resident_db.cpp` + link `-lsqlite3`.

## 9. Kiểm thử

- **Unit (máy dev, không NPU):**
  - `ResidentDB`: open tạo schema; insert resident+embedding; load_active trả đúng; log_event ghi được; touch_resident tăng count.
  - `MatchEngine`: build từ vector giả; match trả đúng resident có max cosine; dưới ngưỡng → unknown.
  - `InteractionManager`: chuỗi frame giả → CONFIRMED sau đúng streak; cooldown chặn lần 2; unknown sau timeout.
  - `migrate_fdb`: fdb mẫu → SQLite có đúng số residents+embeddings; chạy lại skip/overwrite đúng.
- **Tích hợp (Orange Pi):** chạy `main` với `--resident-db`, đối chiếu `match_events` sau vài lần đi qua camera; xác nhận `.fdb` mode vẫn chạy.
- **Không hồi quy:** build + chạy `enroll_faces`/`add_person` như cũ.

## 10. Rủi ro

| Rủi ro                                  | Giảm thiểu                                                           |
| ---------------------------------------- | ---------------------------------------------------------------------- |
| SQLite I/O làm sụt FPS                 | Async writer thread + batch transaction; luồng frame chỉ push queue. |
| Match tuyến tính chậm khi >10k vector | Flat array cache-friendly; đo thực tế; ANN để version sau.        |
| `home_floor` NOT NULL khi migrate      | Đặt sentinel `0` = "chưa đăng ký tầng" (req §6) + log tổng số để HR sửa. Cabin coi `<=0` là không auto-gọi tầng. |
| Lệch dim giữa fdb và schema (512)     | Kiểm tra dim khi build MatchEngine, lỗi rõ ràng.                   |
