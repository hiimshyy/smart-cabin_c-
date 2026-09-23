#pragma once
// SQLite-backed resident database (spec resident-db-layer, R1).
//
// Source of truth for cabin operation: residents, their embeddings, and an
// audit log of match_events. Schema lives in db/schema.sql and is applied
// automatically on first open of an empty DB.
//
// Threading: match_events + resident stat updates are written by a background
// writer thread so the realtime frame loop never blocks on disk I/O. Callers
// push work via log_event()/touch_resident(); close() flushes and joins.

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

// home_floor sentinel: 0 (or <=0) means "no floor registered yet" — cabin
// greets the resident by name but does NOT auto-call a floor (spec req §6).
constexpr int HOME_FLOOR_UNSET = 0;

struct Resident {
    int64_t     id = -1;
    std::string name;
    std::string apartment;
    int         home_floor = HOME_FLOOR_UNSET;
    std::string language;        // "vi" | "en"
    std::string greeting_name;
    std::string role;            // resident | staff | vip | guest_regular
};

struct EmbeddingRow {
    int64_t            resident_id = -1;
    std::string        source;    // id_photo | cabin | admin
    std::vector<float> vector;    // L2-normalized, size == dim
};

struct MatchEvent {
    int         cabin_id       = 1;
    int64_t     resident_id    = -1;   // <0 => stored as NULL (unknown)
    float       similarity     = -1.0f;
    float       liveness_score = -1.0f; // <0 => stored as NULL (v1)
    std::string action;                 // matched|unknown|cancelled|...
    int         floor_selected = 0;     // 0 => none
    int         latency_ms     = 0;
};

// Raw one-to-one mapping of a `cabins` row (spec cabin-runtime-config). This is
// the DB row as stored (camera_urls is the raw JSON string); resolution +
// validation into effective runtime config happens in cabin_config.{h,cpp}.
// Kept here (not in cabin_config.h) so ResidentDB has no dependency on the
// config module — the dependency flows cabin_config -> resident_db.
struct CabinRow {
    int64_t     id               = 1;
    std::string name;
    std::string location;
    std::string camera_urls;              // raw JSON array string, e.g. ["rtsp://..."]
    std::string elevator_endpoint;
    int         floors_min       = 1;
    int         floors_max       = 30;
    int         gst_latency_ms   = 100;   // schema_version 2 (Nhóm A)
    float       match_thr        = 0.35f;
    int         confirm_streak   = 5;
    int         cooldown_ms      = 3000;
    int         unknown_after_ms = 2000;
    int         reconnect_min_ms = 500;
    int         reconnect_max_ms = 10000;
    bool        found            = false; // false if no cabin with this id
};

// Sparse patch for update_cabin_config(): only fields with the matching flag
// set are written. Used by the REST PATCH path (spec Enroll API) and tests.
struct CabinPatch {
    // camera_urls: raw JSON string to store (already validated by caller).
    bool has_camera_urls = false;       std::string camera_urls;
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

class ResidentDB {
public:
    ResidentDB() = default;
    ~ResidentDB();

    ResidentDB(const ResidentDB&)            = delete;
    ResidentDB& operator=(const ResidentDB&) = delete;

    // Open (or create) the DB. WAL + foreign_keys ON. If the DB has no
    // tables yet, apply the schema from `schema_sql_path`. When
    // `spawn_writer` is true (the default, used by the realtime app) a
    // background thread drains the async log_event/touch_resident queue.
    // Offline tools (enroll/add/migrate) that only use the synchronous write
    // API should pass spawn_writer=false to avoid two threads sharing the
    // sqlite3 handle (see code-review P3-6). Returns false on any failure.
    bool open(const std::string& db_path,
              const std::string& schema_sql_path = "db/schema.sql",
              bool spawn_writer = true);

    // Load all active (active=1) residents plus their embeddings.
    bool load_active(std::vector<Resident>& residents,
                     std::vector<EmbeddingRow>& embeddings);

    // ---- Cabin runtime config (spec cabin-runtime-config) -------------
    // Load the cabins row for `id`. On success returns a CabinRow with
    // found=true; if no such row exists, found=false (caller falls back to
    // defaults, R2.2). Synchronous read.
    CabinRow load_cabin(int64_t id) const;

    // Apply a sparse patch to the cabins row for `id` (only flagged fields are
    // written). Synchronous; used by the REST PATCH path. Values must already
    // be validated by the caller (validate_cabin_patch in cabin_config).
    // Returns true if a row was updated.
    bool update_cabin_config(int64_t id, const CabinPatch& patch);

    // ---- Async (non-blocking) writes ---------------------------------
    // Queue a match event for the background writer.
    void log_event(const MatchEvent& ev);
    // Queue "last_seen_at = now, match_count += 1" for a resident.
    void touch_resident(int64_t resident_id);

    // ---- Phase 2 write API (enroll_faces / add_person) ----------------
    // These run synchronously on the caller thread (enroll tools are offline).
    int64_t upsert_resident(const std::string& name,
                            int home_floor = HOME_FLOOR_UNSET);
    bool    add_embedding(int64_t resident_id,
                          const std::string& source,
                          const std::vector<float>& vec);
    bool    delete_embeddings(int64_t resident_id);

    // Look up a resident id by exact name. Returns -1 if not found.
    // Synchronous; used by migrate_fdb to decide skip vs overwrite.
    int64_t find_resident(const std::string& name) const;

    // Delete a resident row (embeddings cascade via FK). Synchronous.
    // Used by offline tools to drop a resident that ended up with no usable
    // embedding (code-review P3-2).
    bool    remove_resident(int64_t resident_id);

    // Wrap a batch of synchronous writes in one transaction so ~N inserts
    // don't each fsync (code-review P3-1). Offline-tool use only; do not mix
    // with the async writer thread. commit() also used after a failed batch
    // via rollback().
    bool    begin();
    bool    commit();
    bool    rollback();

    // Flush pending async writes and stop the writer thread.
    void close();

    bool is_open() const { return db_ != nullptr; }

private:
    // Background writer plumbing --------------------------------------
    struct WriteJob {
        enum class Kind { Event, Touch } kind;
        MatchEvent event;      // valid when kind == Event
        int64_t    resident_id = -1; // valid when kind == Touch
    };
    void writer_loop();
    void flush_jobs(std::vector<WriteJob>& jobs);

    bool apply_schema(const std::string& schema_sql_path);
    bool has_tables() const;

    // Upgrade an existing DB to the latest schema_version (spec
    // cabin-runtime-config, R1). Reads MAX(version); if below target, runs the
    // incremental ALTER blocks in a transaction, tolerating "duplicate column"
    // (a fresh DB from schema.sql already has the v2 columns). Idempotent.
    bool apply_migrations();
    int  current_schema_version() const;

    sqlite3* db_ = nullptr;

    std::thread             writer_;
    std::mutex              q_mtx_;
    std::condition_variable q_cv_;
    std::vector<WriteJob>   queue_;
    bool                    stop_ = false;
};
