# Design — Lớp config vận hành DB-driven (cabin-runtime-config)

**Spec ID**: `cabin-runtime-config`

## Overview

Config vận hành chuyển từ "chỉ CLI" sang "DB là nguồn chân lý, CLI override". Web ghi vào bảng
`cabins`; app đọc lúc khởi động; systemd restart để áp dụng.

**Nguyên tắc**: mọi giá trị đọc từ DB đều đi qua một hàm resolve + validate duy nhất trước khi biến
thành `CamConfig` / `InteractionConfig` / `match_threshold`. Không chỗ nào tin DB thô.

### Phạm vi v1

- Migration `schema_version` 2 cho bảng `cabins` (cột Nhóm A) và `residents.ext_id` (R1).
- Module `cabin_config`: precedence CLI > DB > default và validate/clamp (R2, R3, R4).
- Khâu nối `main.cpp`: đọc dòng `cabins` theo `--cabin-id` lúc khởi động (R2, R3).
- Backend cho REST API (`load_cabin`, `update_cabin_config`, `validate_cabin_patch`). Bản thân HTTP
  endpoint thuộc spec Enroll API (task 5, đang hoãn).
- Đổi config = ghi DB rồi restart service (restart-to-apply, R5). Không hot-reload ở v1.

## Architecture

### Luồng dữ liệu

```
   ┌───────────┐   HTTPS      ┌──────────┐   127.0.0.1:8080    ┌────────────────────┐
   │  web/app  │ ───────────▶ │  nginx   │ ──────────────────▶ │  REST API layer    │
   │ (đội web)│              │ (trên Pi)│    Bearer token     │ (cpp-httplib,      │
   └───────────┘              └──────────┘                     │  spec Enroll API)  │
                                                               └─────────┬──────────┘
                                          GET/PATCH /api/v1/cabins/{id}   │
                                                                          ▼
                                                          ┌───────────────────────────┐
                                                          │  cabins (SQLite/WAL)      │
                                                          │  camera_urls, match_thr,  │
                                                          │  cooldown_ms, ...         │
                                                          └─────────────┬─────────────┘
                                                                        │ đọc lúc khởi động
                                                                        ▼
   ┌─────────────────────────────────────────────────────────────────────────────────┐
   │  face_recog_app (main.cpp)                                                        │
   │                                                                                   │
   │   resolve_cabin_config(cli, db_row, defaults)  →  CabinConfig (hiệu lực)          │
   │        │                                                                          │
   │        ├──▶ CamConfig.pipeline  (từ camera_urls[0] qua build_gst_pipeline)        │
   │        ├──▶ InteractionConfig   (confirm_streak, cooldown_ms, unknown_after_ms)   │
   │        ├──▶ match_threshold                                                       │
   │        └──▶ reconnect backoff min/max                                             │
   └───────────────────────────────────────────────────────────────────────────────┘
                                          ▲
                             systemd Restart=always (áp config mới sau restart — R5)
```

### Thứ tự khởi động trong `main.cpp` (R2, R3)

Thứ tự khởi động mới, chèn sau khi mở `resident_db` thành công (trước khi dựng `CamConfig`):

```
parse CLI  →  set CliOverrides (cờ + giá trị cho từng field người dùng truyền)
             ↓
open resident_db (--resident-db)
             ↓
CabinRow row = resident_db.load_cabin(cabin_id);   // R2.1; row.found=false nếu thiếu → R2.2
             ↓
CabinConfig cfg = resolve_cabin_config(cli, row, DEFAULTS);   // R3 precedence + R4 validate
             ↓
// dựng CamConfig từ cfg thay vì từ CLI trực tiếp
if (cfg.use_stream) {
    cam_cfg.is_stream = true;
    cam_cfg.pipeline  = build_gst_pipeline(cfg.rtsp_url, cfg.gst_latency_ms);  // dùng lại hàm sẵn có
} else {
    cam_cfg.is_stream = false; cam_cfg.cam_id = cam_id;
}
cam_cfg.backoff_min_ms = cfg.reconnect_min_ms;
cam_cfg.backoff_max_ms = cfg.reconnect_max_ms;
             ↓
InteractionConfig icfg { cfg.confirm_streak, cfg.cooldown_ms, cfg.unknown_after_ms, /*reap*/ };
match_threshold = cfg.match_thr;
             ↓
LOG_INFO config hiệu lực + nguồn từng field (cli/db/default)   // R3.3, NFR quan sát được
```

`--face-db` (test/dev) KHÔNG chạy nhánh `load_cabin` (R3.4): giữ đường CLI>default như hiện tại.

`--gst-pipeline` custom vẫn được phép qua CLI (dev), nhưng **không** đến từ DB/web (Nhóm C, R4.6).

### Luồng đổi RTSP end-to-end (R5)

```
1. Người vận hành đổi camera trên web  →  PATCH /api/v1/cabins/1 {camera_urls:["rtsp://mới"]}
2. API validate URL (scheme rtsp) → update cabins.camera_urls → 200 {restart_required:true}
3. Web hiển thị "Đã lưu. Khởi động lại thiết bị để áp dụng."
4. Người vận hành (hoặc web gọi endpoint restart — tương lai) → systemctl restart face-cabin
5. systemd start lại → main.cpp load_cabin đọc URL mới → capture_worker mở RTSP mới
```

Gián đoạn: thời gian app khởi động lại + init NPU (~vài giây). Chấp nhận ở v1 vì đổi camera là thao
tác hiếm (chỉ khi lắp/thay thiết bị). Residents + match_events không đụng tới.

**Tương lai (ngoài scope)** — hot-reload: thêm cột/tín hiệu "config_dirty", `capture_worker` kiểm tra
giữa các vòng, release + `open_capture()` với URL mới. Cần lock cho `CamConfig` chia sẻ 2 thread. Chỉ
làm nếu vận hành thấy restart gây phiền.

## Components and Interfaces

### Bản đồ file

| File | Thay đổi |
|---|---|
| `db/schema.sql` | Migration version 2: `ALTER TABLE cabins ADD COLUMN ...` + `residents.ext_id` + bump `schema_version` |
| `src/resident_db.{h,cpp}` | Migration runtime (v1→v2); API `load_cabin(id)` trả struct `CabinRow`; `update_cabin_config()` cho REST API ghi |
| `src/cabin_config.{h,cpp}` (mới) | Struct `CabinConfig` + `resolve_cabin_config()` (precedence CLI>DB>default) + validate/clamp |
| `src/main.cpp` | Sau khi mở `--resident-db`, gọi `load_cabin` + `resolve_cabin_config`; dùng kết quả thay CLI trực tiếp; log config hiệu lực |
| REST API (spec Enroll API) | Thêm `GET/PATCH /api/v1/cabins/{id}` dùng `load_cabin`/`update_cabin_config` |

> **Ghi chú triển khai (khác design gốc):** `struct CabinRow` (và `CabinPatch`) được đặt trong
> `src/resident_db.h` thay vì `src/cabin_config.h` như phác thảo ban đầu. Lý do: `load_cabin()` /
> `update_cabin_config()` thuộc `ResidentDB`, nên `CabinRow` phải nằm ở tầng resident_db; đặt nó ở
> cabin_config.h sẽ khiến resident_db phụ thuộc ngược vào cabin_config. Chiều phụ thuộc đúng là
> `cabin_config.h` include `resident_db.h` (dùng `CabinRow`/`CabinPatch`), không ngược lại.
> `cabin_config.h` chỉ giữ `CabinConfig` (đã resolve) + `CliOverrides`.

Module `cabin_config` **tách khỏi NPU** (chỉ cần sqlite3 + std) → test độc lập nhanh bằng g++ +
sqlite3 ngay trên Orange Pi (không cần load model), giống các
module khác của `resident-db-layer`.

### `ResidentDB` (`src/resident_db.{h,cpp}`)

```cpp
bool     open(const std::string& db_path,
              const std::string& schema_sql_path = "db/schema.sql",
              bool spawn_writer = true);              // gọi apply_migrations() sau khi có bảng
CabinRow load_cabin(int64_t id) const;                 // found=false nếu không có dòng (R2.2)
bool     update_cabin_config(int64_t id, const CabinPatch& patch);  // chỉ ghi field có cờ
// private
bool apply_migrations();
int  current_schema_version() const;                   // MAX(version), 0 nếu rỗng
```

#### Áp migration runtime (R1.5, R1.6)

`ResidentDB::open()` sau khi đảm bảo bảng tồn tại (version 1) sẽ chạy `apply_migrations()`:

```cpp
// Pseudocode
int cur = current_schema_version();     // MAX(version) trong schema_version, 0 nếu rỗng
if (cur < 2) {
    // Chạy khối ALTER version 2. Bọc trong transaction.
    // Mỗi ALTER ADD COLUMN có thể ném "duplicate column" nếu DB đã có cột
    // (ví dụ DB tạo mới từ schema.sql version mới) → nuốt lỗi duplicate,
    // fail thật với lỗi khác.
    begin();
    exec_ignore_duplicate_column(kMigrationV2Sql);
    exec("INSERT OR IGNORE INTO schema_version(version) VALUES(2)");
    commit();
}
```

Xử lý 2 tình huống:
- **DB cũ (chỉ có version 1)**: các cột chưa tồn tại → `ADD COLUMN` chạy, dữ liệu residents giữ nguyên.
- **DB mới (schema.sql đã chứa cột v2)**: `ADD COLUMN` ném duplicate → nuốt; `schema_version` đã có 2.

### `cabin_config` (`src/cabin_config.{h,cpp}`)

```cpp
std::string parse_first_camera_url(const std::string& camera_urls_json);  // "" nếu rỗng/lỗi
bool        is_allowed_camera_url(const std::string& url);                // rtsp/http/https
CabinConfig resolve_cabin_config(const CliOverrides& cli,
                                 const CabinRow& db,      // db.found=false → bỏ qua tầng DB
                                 const CabinConfig& def);
std::vector<std::string> validate_cabin_patch(const CabinPatch& patch);   // rỗng = hợp lệ
```

#### Precedence (R3)

Cho mỗi trường: `cli nếu cli.has_<field>==true` → ngược lại `db.<field> nếu db.found` → ngược lại `def`.
Đây là lý do cần `CliOverrides`: phải phân biệt "user không truyền `--match-thr`" với "user truyền
`--match-thr 0.35` trùng default" (R3.2). Cách hiện tại trong `main.cpp` khởi tạo biến bằng default
rồi so sánh không phân biệt được — nên khi parse CLI sẽ set cờ `cli.has_<field>=true`.

#### Validate/clamp (R4) — áp NGAY trong resolve

| Trường | Ràng buộc | Vi phạm |
|---|---|---|
| `match_thr` | [0.05, 0.95] | clamp + LOG_WARN |
| `confirm_streak` | ≥ 1 | clamp lên 1 |
| `cooldown_ms`, `unknown_after_ms`, `gst_latency_ms` | ≥ 0 | clamp lên 0 |
| `reconnect_min_ms` | ≥ 1, ≤ max | nếu > max → hoán đổi + LOG_WARN |
| `floors_min/max` | min ≤ max | nếu min > max → LOG_WARN, dùng def |
| `camera_urls[0]` | scheme ∈ {rtsp,http,https} hoặc rỗng | scheme lạ → bỏ, fallback USB + LOG_WARN |

Cùng bộ validate này REST API gọi lại trước khi ghi DB (R4.7) — đặt trong `cabin_config.cpp` để dùng
chung, tránh 2 nơi validate lệch nhau.

#### Parse `camera_urls` (R2.3)

`camera_urls` là JSON array chuỗi. v1 chỉ cần phần tử [0]. Để không kéo lib JSON nặng, parse tối giản:
tách chuỗi trong dấu `["..."]`, lấy chuỗi đầu. Nếu dự án đã có header JSON (kiểm tra khi code), dùng
lại. Rỗng/parse lỗi → `use_stream=false`, fallback USB cam_id (R2.3).

### REST API cho config (thuộc spec Enroll API)

```
GET   /api/v1/cabins/{id}     → JSON các trường Nhóm A hiện tại (không trả Nhóm C)
PATCH /api/v1/cabins/{id}     → body JSON, chỉ chấp nhận trường Nhóm A;
                                validate R4 → update_cabin_config() → ghi cabins
                                response: {ok, restart_required:true, cabin:{...}}
```

- PATCH gặp trường Nhóm C (model path, log, gst-pipeline) → **400** (R4.7).
- PATCH `camera_urls` với phần tử là chuỗi pipeline GStreamer / scheme lạ → **400** (R4.6).
- `restart_required: true` luôn trả về (v1 restart-to-apply, R5.2) để web báo người dùng.

## Data Models

### Migration schema_version 2 (R1)

Thêm vào `db/schema.sql`, chạy sau khối version 1. Dùng `ALTER TABLE ADD COLUMN` nên giữ nguyên
dữ liệu cũ. SQLite `ADD COLUMN` yêu cầu default là hằng — mọi cột đặt default = default CLI hiện tại.

```sql
-- ==== schema_version 2: cabin runtime config + ext_id (DEVELOPMENT_PLAN §3.5) ====
-- ALTER TABLE ... ADD COLUMN là idempotent-unsafe (chạy lại sẽ lỗi "duplicate column"),
-- nên migration chạy CÓ ĐIỀU KIỆN: chỉ khi schema_version < 2 (xem apply_migrations()).

ALTER TABLE cabins ADD COLUMN gst_latency_ms   INTEGER NOT NULL DEFAULT 100;
ALTER TABLE cabins ADD COLUMN match_thr        REAL    NOT NULL DEFAULT 0.35;
ALTER TABLE cabins ADD COLUMN confirm_streak   INTEGER NOT NULL DEFAULT 5;
ALTER TABLE cabins ADD COLUMN cooldown_ms      INTEGER NOT NULL DEFAULT 3000;
ALTER TABLE cabins ADD COLUMN unknown_after_ms INTEGER NOT NULL DEFAULT 2000;
ALTER TABLE cabins ADD COLUMN reconnect_min_ms INTEGER NOT NULL DEFAULT 500;
ALTER TABLE cabins ADD COLUMN reconnect_max_ms INTEGER NOT NULL DEFAULT 10000;

ALTER TABLE residents ADD COLUMN ext_id TEXT;   -- cloud id (ElevCore), nullable
CREATE UNIQUE INDEX IF NOT EXISTS idx_residents_ext_id
    ON residents(ext_id) WHERE ext_id IS NOT NULL;

INSERT OR IGNORE INTO schema_version (version) VALUES (2);
```

`camera_urls`, `elevator_endpoint`, `floors_min`, `floors_max` **đã có** ở version 1 → không thêm.

> Ghi chú: để đơn giản, `db/schema.sql` mới sẽ chứa CẢ định nghĩa cột (dạng `CREATE TABLE cabins` đầy
> đủ cột) cho DB tạo-mới, VÀ khối ALTER có điều kiện cho DB nâng cấp. `apply_migrations()` bảo đảm
> đường nâng cấp; `CREATE TABLE ... IF NOT EXISTS` lo đường tạo-mới.

### `CabinRow` và `CabinPatch` (`src/resident_db.h`)

```cpp
struct CabinRow {              // ánh xạ 1-1 dòng cabins đọc từ DB (raw, chưa validate)
    int64_t     id               = 1;
    std::string name;
    std::string location;
    std::string camera_urls;     // chuỗi JSON array thô, ví dụ: ["rtsp://..."]
    std::string elevator_endpoint;
    int         floors_min       = 1;
    int         floors_max       = 30;
    int         gst_latency_ms   = 100;    // schema_version 2 (Nhóm A)
    float       match_thr        = 0.35f;
    int         confirm_streak   = 5;
    int         cooldown_ms      = 3000;
    int         unknown_after_ms = 2000;
    int         reconnect_min_ms = 500;
    int         reconnect_max_ms = 10000;
    bool        found            = false;  // false nếu không có dòng cabin id này
};

// Patch thưa cho update_cabin_config(): chỉ ghi field có cờ has_<field>=true.
struct CabinPatch {
    bool has_camera_urls = false;       std::string camera_urls;   // đã validate bởi caller
    bool has_elevator_endpoint = false; std::string elevator_endpoint;
    bool has_floors_min = false;        int floors_min = 1;
    bool has_floors_max = false;        int floors_max = 30;
    bool has_gst_latency_ms = false;    int gst_latency_ms = 100;
    bool has_match_thr = false;         float match_thr = 0.35f;
    bool has_confirm_streak = false;    int confirm_streak = 5;
    bool has_cooldown_ms = false;       int cooldown_ms = 3000;
    bool has_unknown_after_ms = false;  int unknown_after_ms = 2000;
    bool has_reconnect_min_ms = false;  int reconnect_min_ms = 500;
    bool has_reconnect_max_ms = false;  int reconnect_max_ms = 10000;
};
```

### `CabinConfig` và `CliOverrides` (`src/cabin_config.h`)

```cpp
// Config đã resolve + validate, sẵn sàng nạp vào pipeline. Giá trị khởi tạo = default compile-in.
struct CabinConfig {
    std::string rtsp_url;                  // camera_urls[0], "" nếu dùng USB
    bool        use_stream       = false;
    int         gst_latency_ms   = 100;
    float       match_thr        = 0.35f;
    int         confirm_streak   = 5;
    double      cooldown_ms      = 3000.0;
    double      unknown_after_ms = 2000.0;
    int         reconnect_min_ms = 500;
    int         reconnect_max_ms = 10000;
    int         floors_min       = 1;
    int         floors_max       = 30;
    std::string elevator_endpoint;
};

// Cờ đánh dấu tham số nào người dùng THỰC SỰ truyền trên CLI (R3.2).
// Giá trị kèm theo chỉ được đọc khi cờ tương ứng = true.
struct CliOverrides {
    bool has_source         = false;  std::string source_val;
    bool has_gst_latency    = false;  int    gst_latency_val    = 100;
    bool has_match_thr      = false;  float  match_thr_val      = 0.35f;
    bool has_confirm_streak = false;  int    confirm_streak_val = 5;
    bool has_cooldown       = false;  double cooldown_val       = 3000.0;
    bool has_unknown_after  = false;  double unknown_after_val  = 2000.0;
    bool has_reconnect_min  = false;  int    reconnect_min_val  = 500;
    bool has_reconnect_max  = false;  int    reconnect_max_val  = 10000;
};
```

`floors_min/max` và `elevator_endpoint` không có cờ CLI: chỉ lấy từ DB hoặc default.

### JSON của REST API

- `GET` trả các trường Nhóm A của `CabinRow`: `camera_urls`, `elevator_endpoint`, `floors_min`,
  `floors_max`, `gst_latency_ms`, `match_thr`, `confirm_streak`, `cooldown_ms`, `unknown_after_ms`,
  `reconnect_min_ms`, `reconnect_max_ms`. Không trả trường Nhóm C.
- `PATCH` nhận một tập con các trường trên; mỗi trường có mặt ánh xạ sang cờ `has_<field>` của
  `CabinPatch`. `id` không sửa được qua PATCH.
- Response PATCH: `{ok, restart_required: true, cabin: {...}}`.

## *Correctness Properties

*Property là đặc tính phải đúng trên mọi lần chạy hợp lệ của hệ thống. Mỗi property nối một yêu cầu
trong [requirements.md](requirements.md) với cách kiểm chứng ở mục Testing Strategy.*

Trong dòng **Validates**, `Rx.y` là Requirement `x`, tiêu chí chấp nhận thứ `y` ở mục "Requirements"
của requirements.md; "NFR" là mục "Yêu cầu phi chức năng".

### *Property 1: Migration giữ dữ liệu và idempotent

*Với mọi* DB ở version 1, sau `ResidentDB::open()` bảng `cabins` có đủ 7 cột Nhóm A mới, `residents`
có cột `ext_id` cùng index `idx_residents_ext_id`, và `MAX(version)` trong `schema_version` ≥ 2. Mọi
dòng `residents`, `embeddings`, `match_events` và giá trị cột v1 của `cabins` giữ nguyên. Mở lại cùng
DB không lỗi và không đổi dữ liệu. DB tạo mới từ `db/schema.sql` mở không lỗi duplicate column.

**Validates: Requirements R1.1, R1.2, R1.3, R1.4, R1.5, R1.6, NFR không hồi quy**

### *Property 2: Precedence theo từng field

*Với mọi* field Nhóm A có cờ CLI và mọi tổ hợp (cờ `has_<field>`, `db.found`), giá trị trước bước
validate bằng giá trị CLI khi cờ bật, ngược lại bằng giá trị DB khi `db.found`, ngược lại bằng
default. Giá trị CLI trùng default nhưng không có cờ không bao giờ đè giá trị DB.

**Validates: Requirements R3.1, R3.2**

### *Property 3: Thiếu dòng cabin không làm app dừng

*Với mọi* `cabin_id` không có trong bảng `cabins`, `load_cabin` trả `found=false`, config hiệu lực chỉ
lấy từ CLI và default, app log cảnh báo kèm id số và tiếp tục khởi động.

**Validates: Requirements R2.2**

### *Property 4: Config hiệu lực luôn thỏa ràng buộc

*Với mọi* input của `resolve_cabin_config` (kể cả DB bị sửa tay thành giá trị bẩn), output thỏa:
`match_thr` ∈ [0.05, 0.95], `confirm_streak` ≥ 1, `cooldown_ms`, `unknown_after_ms`,
`gst_latency_ms` ≥ 0, 1 ≤ `reconnect_min_ms` ≤ `reconnect_max_ms`, `floors_min` ≤ `floors_max`. Mỗi
lần kẹp, hoán đổi hoặc thay bằng default đều có một LOG_WARN.

**Validates: Requirements R2.4, R4.1, R4.2, R4.3, R4.4, R4.5**

### *Property 5: Nguồn camera từ DB chỉ là URL hợp lệ

*Với mọi* `camera_urls` đọc từ DB (khi không có `--source`), `use_stream=true` khi và chỉ khi phần tử
đầu tiên có scheme `rtsp://`, `http://` hoặc `https://`. Mảng rỗng, chuỗi parse lỗi, scheme lạ hoặc
chuỗi pipeline GStreamer cho `use_stream=false` và fallback USB. Pipeline luôn được dựng bằng
`build_gst_pipeline()`, không nối chuỗi DB thô. `--source` trên CLI là override dev và không qua bước
kiểm scheme này.

**Validates: Requirements R2.3, R4.6, NFR an toàn**

### *Property 6: App và REST dùng chung luật validate

*Với mọi* `CabinPatch`, `validate_cabin_patch` trả danh sách lỗi rỗng chỉ khi mọi field có mặt nằm
trong miền mà `resolve_cabin_config` không kẹp, và `camera_urls` (nếu có) có phần tử đầu hợp lệ
theo Property 5. Ràng buộc liên field (`reconnect_min_ms` ≤ `reconnect_max_ms`, `floors_min` ≤
`floors_max`) chỉ được kiểm khi patch chứa cả hai đầu; khi patch chỉ có một đầu, resolve lúc khởi
động là chốt cuối.

**Validates: Requirements R4.1, R4.2, R4.3, R4.4, R4.5, R4.6, R4.7**

### *Property 7: PATCH chỉ ghi cột Nhóm A của đúng một cabin

*Với mọi* `CabinPatch`, `update_cabin_config(id, patch)` chỉ ghi các cột có cờ `has_<field>` trên dòng
`cabins` có `id` đó. Các dòng cabin khác, các cột không có cờ, và dữ liệu `residents`, `embeddings`,
`match_events` không đổi. REST từ chối trường Nhóm C bằng 400 trước khi ghi, và mọi response PATCH
thành công có `restart_required: true`.

**Validates: Requirements R4.7, R5.1, R5.2, R5.4**

### *Property 8: Chế độ `--face-db` không đọc `cabins`

*Với mọi* lần chạy không có cặp `--recog-model` + `--resident-db` (gồm chế độ `--face-db`), app không
gọi `load_cabin`; config hiệu lực chỉ theo CLI > default.

**Validates: Requirements R3.4**

### *Property 9: Log khởi động quan sát được và không lộ dữ liệu nhạy cảm

*Với mọi* lần khởi động có đọc `cabins`, app ghi một dòng LOG_INFO tóm tắt config hiệu lực kèm nguồn
(cli/db/default). Dòng này chỉ chứa cabin id số, giá trị config và nhãn camera đã che
(`configured(redacted)` hoặc `usb`), không lộ URL/credential RTSP, không có PII.

**Validates: Requirements R3.3, NFR quan sát được**

## Error Handling

Nguyên tắc: lỗi config không làm app crash. Khi thiếu hoặc sai dữ liệu, app kẹp về miền hợp lệ hoặc
dùng default và log cảnh báo. Lỗi đi theo convention của repo (`bool` + log qua `src/log/logger.h`),
không throw. Log chỉ chứa id số, không chứa URL camera hay PII.

| Tình huống | Xử lý |
|---|---|
| Không mở được resident DB để đọc config (`main.cpp`) | LOG_WARN, dùng CLI/default, app vẫn chạy |
| `--gst-pipeline` được truyền trên CLI | `main.cpp` bỏ qua `load_cabin`; mọi field theo CLI/default |
| Migration gặp lỗi `duplicate column name` | Nuốt lỗi (DB tạo mới đã có cột), tiếp tục |
| Migration gặp lỗi SQL khác | `ROLLBACK`, LOG_ERROR, `ResidentDB::open()` trả `false` |
| Migration chạy lại trên DB đã ở version ≥ 2 | Không chạy khối ALTER (điều kiện `schema_version < 2`) |
| Cabin id không có trong DB | `row.found=false` → dùng default + LOG_WARN, không crash (R2.2) |
| `camera_urls` rỗng hoặc parse lỗi | `use_stream=false`, fallback USB `cam_id` (R2.3) |
| `camera_urls[0]` có scheme lạ hoặc là chuỗi pipeline GStreamer | Bỏ URL, fallback USB + LOG_WARN (R4.6) |
| DB bị sửa tay thành giá trị bẩn (ví dụ `match_thr=5.0`) | resolve luôn validate/clamp + LOG_WARN (R4); không tin DB thô |
| `reconnect_min_ms` > `reconnect_max_ms` | Hoán đổi + LOG_WARN |
| `floors_min` > `floors_max` | Dùng default + LOG_WARN |
| CLI default vô tình đè DB | `CliOverrides` cờ per-field, chỉ override khi user thực truyền (R3.2) |
| PATCH chứa trường Nhóm C (model path, log, gst-pipeline) | REST trả 400, không ghi DB (R4.7) |
| PATCH `camera_urls` rỗng, scheme lạ hoặc là chuỗi pipeline | `validate_cabin_patch` trả lỗi → REST 400 (R4.6) |
| PATCH có giá trị số ngoài miền | `validate_cabin_patch` trả lỗi → REST 400 |
| PATCH chỉ có một đầu của cặp min/max | Không kiểm liên field; resolve lúc khởi động hoán đổi hoặc dùng default |
| `update_cabin_config` với id không tồn tại | Trả `false` (không có dòng nào được cập nhật). Mã HTTP REST trả về cho trường hợp này chưa chốt (spec Enroll API) |
| Đổi RTSP làm mất residents | Chỉ ghi cột `cabins`, không đụng residents/embeddings (R5.4) |

## Testing Strategy

Mỗi test là một `main()` riêng, đăng ký trong `tests/run_tests.sh` và chạy bằng
`bash tests/run_tests.sh`. Test dùng SQLite chỉ chạy khi có header sqlite3 (fallback:
`bash tests/_setup_sqlite_local.sh`). Số trong ngoặc là property mà test kiểm.

### Unit tests

- `test_cabin_config` (khối `== CabinConfig ==` trong `run_tests.sh`): precedence 3 tổ hợp
  cli/db/default; clamp `match_thr` hai đầu, `confirm_streak`, hoán đổi reconnect min > max, floors
  min > max; `parse_first_camera_url` với URL rtsp, mảng rỗng, chuỗi rỗng; `is_allowed_camera_url`
  cho phép rtsp/http và từ chối pipeline/chuỗi rỗng; `camera_urls` từ DB là pipeline → fallback USB;
  `validate_cabin_patch` với rtsp hợp lệ, chuỗi pipeline, `match_thr` ngoài miền,
  `confirm_streak` < 1, reconnect min > max (Property 2, 4, 5, 6).
- `test_resident_db`: DB tạo mới từ `db/schema.sql` có dòng `cabins` mặc định (một phần Property 1).

### Build và smoke test

- `make -j4` trên Orange Pi build đủ 6 binary (task 4.4).
- Smoke test chạy thật gộp vào đợt test camera đang hoãn: (a) DB có RTSP → app mở RTSP; (b) CLI
  `--source` override DB; (c) cabin id thiếu → default + warn (Property 2, 3, 5).
- Test REST (task 5.3) làm cùng spec Enroll API: PATCH `camera_urls` hợp lệ → DB đổi +
  `restart_required`; PATCH model path → 400; PATCH chuỗi pipeline → 400 (Property 6, 7).

### Khoảng trống đã biết

- Chưa có test nào trong `tests/` gọi `load_cabin`, `update_cabin_config`, hoặc mở một DB version 1
  để chạy migration v1→v2, dù tasks 1.3 và 2.3 đã đánh dấu xong. Property 1 (đường nâng cấp), 3 và 7
  chưa có test tự động.
- Chưa có test tự động cho Property 8 và 9. Dòng LOG_INFO khởi động hiện chỉ có một tag `[src: ...]`
  phản ánh nguồn camera, chưa ghi nguồn từng field như R3.3 yêu cầu.
