# Design — Lớp config vận hành DB-driven (cabin-runtime-config)

**Spec ID**: `cabin-runtime-config`

## 1. Kiến trúc tổng thể

Config vận hành chuyển từ "chỉ CLI" sang "DB là nguồn chân lý, CLI override". Web ghi vào bảng
`cabins`; app đọc lúc khởi động; systemd restart để áp dụng.

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

**Nguyên tắc**: mọi giá trị đọc từ DB đều đi qua một hàm resolve + validate duy nhất trước khi biến
thành `CamConfig` / `InteractionConfig` / `match_threshold`. Không chỗ nào tin DB thô.

## 2. Thay đổi & file

| File | Thay đổi |
|---|---|
| `db/schema.sql` | Migration version 2: `ALTER TABLE cabins ADD COLUMN ...` + `residents.ext_id` + bump `schema_version` |
| `src/resident_db.{h,cpp}` | Migration runtime (v1→v2); API `load_cabin(id)` trả struct `CabinRow`; `update_cabin_config()` cho REST API ghi |
| `src/cabin_config.{h,cpp}` (mới) | Struct `CabinConfig` + `resolve_cabin_config()` (precedence CLI>DB>default) + validate/clamp |
| `src/main.cpp` | Sau khi mở `--resident-db`, gọi `load_cabin` + `resolve_cabin_config`; dùng kết quả thay CLI trực tiếp; log config hiệu lực |
| REST API (spec Enroll API) | Thêm `GET/PATCH /api/v1/cabins/{id}` dùng `load_cabin`/`update_cabin_config` |

Module `cabin_config` **tách khỏi NPU** (chỉ cần sqlite3 + std) → test được trên máy dev, giống các
module khác của `resident-db-layer`.

## 3. Migration schema_version 2 (R1)

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

### 3.1 Áp migration runtime (R1.5, R1.6)

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

> Ghi chú: để đơn giản, `db/schema.sql` mới sẽ chứa CẢ định nghĩa cột (dạng `CREATE TABLE cabins` đầy
> đủ cột) cho DB tạo-mới, VÀ khối ALTER có điều kiện cho DB nâng cấp. `apply_migrations()` bảo đảm
> đường nâng cấp; `CREATE TABLE ... IF NOT EXISTS` lo đường tạo-mới.

## 4. `CabinConfig` + resolve (R2, R3, R4)

```cpp
// src/cabin_config.h
struct CabinRow {              // ánh xạ 1-1 dòng cabins đọc từ DB (raw, chưa validate)
    int64_t     id             = 1;
    std::string name;
    std::string camera_urls;   // chuỗi JSON array thô, ví dụ: ["rtsp://..."]
    std::string elevator_endpoint;
    int         floors_min     = 1;
    int         floors_max     = 30;
    int         gst_latency_ms = 100;
    float       match_thr      = 0.35f;
    int         confirm_streak = 5;
    int         cooldown_ms    = 3000;
    int         unknown_after_ms = 2000;
    int         reconnect_min_ms = 500;
    int         reconnect_max_ms = 10000;
    bool        found          = false;   // false nếu không có dòng cabin id này
};

// Config đã resolve + validate, sẵn sàng nạp vào pipeline.
struct CabinConfig {
    std::string rtsp_url;          // camera_urls[0], "" nếu dùng USB
    bool        use_stream = false;
    int         gst_latency_ms;
    float       match_thr;
    int         confirm_streak;
    double      cooldown_ms;
    double      unknown_after_ms;
    int         reconnect_min_ms;
    int         reconnect_max_ms;
    int         floors_min, floors_max;
    std::string elevator_endpoint;
};

// Cờ đánh dấu tham số nào người dùng THỰC SỰ truyền trên CLI (R3.2).
struct CliOverrides {
    bool source=false, gst_latency=false, match_thr=false,
         confirm_streak=false, cooldown=false, unknown_after=false,
         reconnect_min=false, reconnect_max=false;
    // giá trị CLI kèm theo (chỉ đọc khi cờ = true)
    std::string source_val; int gst_latency_val; float match_thr_val; /* ... */
};

CabinConfig resolve_cabin_config(const CliOverrides& cli,
                                 const CabinRow& db,      // db.found=false → bỏ qua tầng DB
                                 const CabinConfig& def);
```

### 4.1 Precedence (R3)

Cho mỗi trường: `cli nếu cli.<field>==true` → ngược lại `db.<field> nếu db.found` → ngược lại `def`.
Đây là lý do cần `CliOverrides`: phải phân biệt "user không truyền `--match-thr`" với "user truyền
`--match-thr 0.35` trùng default" (R3.2). Cách hiện tại trong `main.cpp` khởi tạo biến bằng default
rồi so sánh không phân biệt được — nên khi parse CLI sẽ set cờ `cli.<field>=true`.

### 4.2 Validate/clamp (R4) — áp NGAY trong resolve

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

### 4.3 Parse `camera_urls` (R2.3)

`camera_urls` là JSON array chuỗi. v1 chỉ cần phần tử [0]. Để không kéo lib JSON nặng, parse tối giản:
tách chuỗi trong dấu `["..."]`, lấy chuỗi đầu. Nếu dự án đã có header JSON (kiểm tra khi code), dùng
lại. Rỗng/parse lỗi → `use_stream=false`, fallback USB cam_id (R2.3).

## 5. Khâu nối `main.cpp` (R2, R3)

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

## 6. REST API cho config (thuộc spec Enroll API, mô tả dữ liệu ở đây)

```
GET   /api/v1/cabins/{id}     → JSON các trường Nhóm A hiện tại (không trả Nhóm C)
PATCH /api/v1/cabins/{id}     → body JSON, chỉ chấp nhận trường Nhóm A;
                                validate R4 → update_cabin_config() → ghi cabins
                                response: {ok, restart_required:true, cabin:{...}}
```

- PATCH gặp trường Nhóm C (model path, log, gst-pipeline) → **400** (R4.7).
- PATCH `camera_urls` với phần tử là chuỗi pipeline GStreamer / scheme lạ → **400** (R4.6).
- `restart_required: true` luôn trả về (v1 restart-to-apply, R5.2) để web báo người dùng.

## 7. Luồng đổi RTSP end-to-end (R5)

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

## 8. Rủi ro & đối phó

| Rủi ro | Đối phó |
|---|---|
| DB bị sửa tay thành giá trị bẩn (match_thr=5.0) | resolve luôn validate/clamp (R4); không tin DB thô |
| Web nhét pipeline GStreamer vào camera_urls | validate scheme, từ chối 400 (R4.6); app chỉ nhận URL |
| Migration chạy lại lỗi duplicate column | apply có điều kiện `schema_version < 2` + nuốt lỗi duplicate |
| CLI default vô tình đè DB | `CliOverrides` cờ per-field, chỉ override khi user thực truyền (R3.2) |
| Đổi RTSP làm mất residents | chỉ ghi cột `cabins`, không đụng residents/embeddings (R5.4) |
| Cabin id không có trong DB | row.found=false → dùng default + LOG_WARN, không crash (R2.2) |
