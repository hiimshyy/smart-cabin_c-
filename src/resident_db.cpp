#include "resident_db.h"
#include "log/logger.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

// --------------------------------------------------------------------------
// Small RAII / helper utilities
// --------------------------------------------------------------------------
namespace {

bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "SQL error: %s", err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------------------
ResidentDB::~ResidentDB() {
    close();
}

// --------------------------------------------------------------------------
// Task 3.1 — open()
// --------------------------------------------------------------------------
bool ResidentDB::open(const std::string& db_path,
                      const std::string& schema_sql_path,
                      bool spawn_writer) {
    if (db_) {
        LOG_ERROR("db", "already open");
        return false;
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "cannot open %s: %s",
                  db_path.c_str(), sqlite3_errmsg(db_));
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }

    // WAL for concurrent read while writer thread commits; FK enforcement.
    if (!exec_sql(db_, "PRAGMA journal_mode=WAL;") ||
        !exec_sql(db_, "PRAGMA foreign_keys=ON;") ||
        !exec_sql(db_, "PRAGMA busy_timeout=5000;")) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    if (!has_tables()) {
        if (!apply_schema(schema_sql_path)) {
            LOG_ERROR("db", "failed to apply schema from %s",
                      schema_sql_path.c_str());
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
        LOG_INFO("db", "applied schema from %s", schema_sql_path.c_str());
    }

    // Upgrade older DBs to the latest schema_version (spec cabin-runtime-config).
    // Runs for both freshly-created and pre-existing DBs; it is idempotent and
    // tolerates already-present columns.
    if (!apply_migrations()) {
        LOG_ERROR("db", "schema migration failed");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Start background writer (skipped for offline tools that only use the
    // synchronous write API — see spawn_writer / code-review P3-6).
    stop_ = false;
    if (spawn_writer) {
        writer_ = std::thread(&ResidentDB::writer_loop, this);
    }
    return true;
}

bool ResidentDB::has_tables() const {
    sqlite3_stmt* st = nullptr;
    const char* q =
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name='residents' LIMIT 1;";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) return false;
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

bool ResidentDB::apply_schema(const std::string& schema_sql_path) {
    std::ifstream f(schema_sql_path, std::ios::binary);
    if (!f) {
        LOG_ERROR("db", "cannot read schema file %s", schema_sql_path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string sql = ss.str();
    return exec_sql(db_, sql.c_str());
}

// --------------------------------------------------------------------------
// Schema migrations (spec cabin-runtime-config, R1)
// --------------------------------------------------------------------------
int ResidentDB::current_schema_version() const {
    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT MAX(version) FROM schema_version;";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) return 0;
    int v = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

namespace {
// Run one statement, swallowing only the "duplicate column name" error that
// happens when the column already exists (fresh DB created from a schema.sql
// that already defines the v2 columns). Any other error is fatal.
bool exec_allow_dup_column(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) { if (err) sqlite3_free(err); return true; }
    std::string msg = err ? err : sqlite3_errmsg(db);
    if (err) sqlite3_free(err);
    if (msg.find("duplicate column name") != std::string::npos) {
        return true;   // column already present — fine
    }
    LOG_ERROR("db", "migration SQL error: %s", msg.c_str());
    return false;
}
}  // namespace

bool ResidentDB::apply_migrations() {
    int cur = current_schema_version();

    // ---- version 2: cabin runtime config columns + residents.ext_id -------
    if (cur < 2) {
        LOG_INFO("db", "migrating schema %d -> 2", cur);
        static const char* kV2Cols[] = {
            "ALTER TABLE cabins ADD COLUMN gst_latency_ms   INTEGER NOT NULL DEFAULT 100;",
            "ALTER TABLE cabins ADD COLUMN match_thr        REAL    NOT NULL DEFAULT 0.35;",
            "ALTER TABLE cabins ADD COLUMN confirm_streak   INTEGER NOT NULL DEFAULT 5;",
            "ALTER TABLE cabins ADD COLUMN cooldown_ms      INTEGER NOT NULL DEFAULT 3000;",
            "ALTER TABLE cabins ADD COLUMN unknown_after_ms INTEGER NOT NULL DEFAULT 2000;",
            "ALTER TABLE cabins ADD COLUMN reconnect_min_ms INTEGER NOT NULL DEFAULT 500;",
            "ALTER TABLE cabins ADD COLUMN reconnect_max_ms INTEGER NOT NULL DEFAULT 10000;",
            "ALTER TABLE residents ADD COLUMN ext_id TEXT;",
        };
        if (!exec_sql(db_, "BEGIN;")) return false;
        for (const char* stmt : kV2Cols) {
            if (!exec_allow_dup_column(db_, stmt)) {
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
        }
        // Partial unique index is created with IF NOT EXISTS (no dup issue).
        if (!exec_sql(db_,
                "CREATE UNIQUE INDEX IF NOT EXISTS idx_residents_ext_id "
                "ON residents(ext_id) WHERE ext_id IS NOT NULL;") ||
            !exec_sql(db_,
                "INSERT OR IGNORE INTO schema_version (version) VALUES (2);")) {
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        if (!exec_sql(db_, "COMMIT;")) return false;
        LOG_INFO("db", "schema now at version %d", current_schema_version());
    }
    return true;
}

// --------------------------------------------------------------------------
// Cabin runtime config (spec cabin-runtime-config, Task 2)
// --------------------------------------------------------------------------
CabinRow ResidentDB::load_cabin(int64_t id) const {
    CabinRow row;
    row.id = id;
    sqlite3_stmt* st = nullptr;
    const char* q =
        "SELECT name, location, camera_urls, elevator_endpoint, "
        "       floors_min, floors_max, gst_latency_ms, match_thr, "
        "       confirm_streak, cooldown_ms, unknown_after_ms, "
        "       reconnect_min_ms, reconnect_max_ms "
        "FROM cabins WHERE id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) {
        LOG_WARN("db", "load_cabin prepare failed: %s", sqlite3_errmsg(db_));
        return row;   // found=false
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        auto text = [&](int c) -> std::string {
            const unsigned char* t = sqlite3_column_text(st, c);
            return t ? reinterpret_cast<const char*>(t) : std::string();
        };
        row.name              = text(0);
        row.location          = text(1);
        row.camera_urls       = text(2);
        row.elevator_endpoint = text(3);
        row.floors_min        = sqlite3_column_int(st, 4);
        row.floors_max        = sqlite3_column_int(st, 5);
        row.gst_latency_ms    = sqlite3_column_int(st, 6);
        row.match_thr         = (float)sqlite3_column_double(st, 7);
        row.confirm_streak    = sqlite3_column_int(st, 8);
        row.cooldown_ms       = sqlite3_column_int(st, 9);
        row.unknown_after_ms  = sqlite3_column_int(st, 10);
        row.reconnect_min_ms  = sqlite3_column_int(st, 11);
        row.reconnect_max_ms  = sqlite3_column_int(st, 12);
        row.found             = true;
    }
    sqlite3_finalize(st);
    return row;
}

bool ResidentDB::update_cabin_config(int64_t id, const CabinPatch& patch) {
    // Build the SET clause from only the flagged fields.
    std::vector<std::string> sets;
    // For bound params we keep a parallel list of (type, value) via lambdas at
    // bind time; simpler: build "col=?" and bind in the same order.
    struct Bind { char kind; int64_t i; double d; std::string s; };
    std::vector<Bind> binds;
    auto add_int  = [&](const char* col, int v)         { sets.push_back(std::string(col) + "=?"); binds.push_back({'i', v, 0, {}}); };
    auto add_real = [&](const char* col, double v)      { sets.push_back(std::string(col) + "=?"); binds.push_back({'d', 0, v, {}}); };
    auto add_text = [&](const char* col, const std::string& v) { sets.push_back(std::string(col) + "=?"); binds.push_back({'s', 0, 0, v}); };

    if (patch.has_camera_urls)       add_text("camera_urls",       patch.camera_urls);
    if (patch.has_elevator_endpoint) add_text("elevator_endpoint", patch.elevator_endpoint);
    if (patch.has_floors_min)        add_int ("floors_min",        patch.floors_min);
    if (patch.has_floors_max)        add_int ("floors_max",        patch.floors_max);
    if (patch.has_gst_latency_ms)    add_int ("gst_latency_ms",    patch.gst_latency_ms);
    if (patch.has_match_thr)         add_real("match_thr",         patch.match_thr);
    if (patch.has_confirm_streak)    add_int ("confirm_streak",    patch.confirm_streak);
    if (patch.has_cooldown_ms)       add_int ("cooldown_ms",       patch.cooldown_ms);
    if (patch.has_unknown_after_ms)  add_int ("unknown_after_ms",  patch.unknown_after_ms);
    if (patch.has_reconnect_min_ms)  add_int ("reconnect_min_ms",  patch.reconnect_min_ms);
    if (patch.has_reconnect_max_ms)  add_int ("reconnect_max_ms",  patch.reconnect_max_ms);

    if (sets.empty()) {
        LOG_WARN("db", "update_cabin_config: empty patch, nothing to do");
        return false;
    }

    std::string sql = "UPDATE cabins SET ";
    for (size_t i = 0; i < sets.size(); ++i) {
        if (i) sql += ", ";
        sql += sets[i];
    }
    sql += " WHERE id=?;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("db", "update_cabin_config prepare failed: %s", sqlite3_errmsg(db_));
        return false;
    }
    int idx = 1;
    for (const auto& b : binds) {
        if      (b.kind == 'i') sqlite3_bind_int64(st, idx, b.i);
        else if (b.kind == 'd') sqlite3_bind_double(st, idx, b.d);
        else                    sqlite3_bind_text(st, idx, b.s.c_str(), -1, SQLITE_TRANSIENT);
        ++idx;
    }
    sqlite3_bind_int64(st, idx, id);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("db", "update_cabin_config step failed: %s", sqlite3_errmsg(db_));
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

// --------------------------------------------------------------------------
// Task 3.2 — load_active()
// --------------------------------------------------------------------------
bool ResidentDB::load_active(std::vector<Resident>& residents,
                             std::vector<EmbeddingRow>& embeddings) {
    if (!db_) return false;
    residents.clear();
    embeddings.clear();

    // ---- residents (active only) ----
    {
        const char* q =
            "SELECT id, name, apartment, home_floor, language, "
            "       greeting_name, role "
            "FROM residents WHERE active=1;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) {
            LOG_ERROR("db", "prepare residents failed: %s", sqlite3_errmsg(db_));
            return false;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            Resident r;
            r.id         = sqlite3_column_int64(st, 0);
            auto txt = [&](int c) -> std::string {
                const unsigned char* p = sqlite3_column_text(st, c);
                return p ? reinterpret_cast<const char*>(p) : std::string();
            };
            r.name          = txt(1);
            r.apartment     = txt(2);
            r.home_floor    = sqlite3_column_int(st, 3);
            r.language      = txt(4);
            r.greeting_name = txt(5);
            r.role          = txt(6);
            residents.push_back(std::move(r));
        }
        sqlite3_finalize(st);
    }

    // ---- embeddings for active residents ----
    {
        const char* q =
            "SELECT e.resident_id, e.source, e.vector "
            "FROM embeddings e "
            "JOIN residents r ON r.id = e.resident_id "
            "WHERE r.active=1;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) {
            LOG_ERROR("db", "prepare embeddings failed: %s", sqlite3_errmsg(db_));
            return false;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            EmbeddingRow e;
            e.resident_id = sqlite3_column_int64(st, 0);
            const unsigned char* src = sqlite3_column_text(st, 1);
            e.source = src ? reinterpret_cast<const char*>(src) : std::string();
            const void* blob = sqlite3_column_blob(st, 2);
            int nbytes       = sqlite3_column_bytes(st, 2);
            int nfloats      = nbytes / static_cast<int>(sizeof(float));
            e.vector.resize(nfloats);
            if (blob && nfloats > 0) {
                std::memcpy(e.vector.data(), blob, nfloats * sizeof(float));
            }
            embeddings.push_back(std::move(e));
        }
        sqlite3_finalize(st);
    }
    return true;
}

// --------------------------------------------------------------------------
// Task 3.3 — async writes
// --------------------------------------------------------------------------
void ResidentDB::log_event(const MatchEvent& ev) {
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        WriteJob j;
        j.kind  = WriteJob::Kind::Event;
        j.event = ev;
        queue_.push_back(std::move(j));
    }
    q_cv_.notify_one();
}

void ResidentDB::touch_resident(int64_t resident_id) {
    if (!db_ || resident_id < 0) return;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        WriteJob j;
        j.kind        = WriteJob::Kind::Touch;
        j.resident_id = resident_id;
        queue_.push_back(std::move(j));
    }
    q_cv_.notify_one();
}

void ResidentDB::writer_loop() {
    using namespace std::chrono_literals;
    std::vector<WriteJob> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(q_mtx_);
            // Wake on new work, stop, or every 500ms to flush.
            q_cv_.wait_for(lk, 500ms, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty() && stop_) break;
            batch.swap(queue_);
        }
        if (!batch.empty()) {
            flush_jobs(batch);
            batch.clear();
        }
    }
    // Drain anything left after stop.
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        batch.swap(queue_);
    }
    if (!batch.empty()) flush_jobs(batch);
}

void ResidentDB::flush_jobs(std::vector<WriteJob>& jobs) {
    if (!db_ || jobs.empty()) return;

    const char* ins_event =
        "INSERT INTO match_events "
        "(cabin_id, resident_id, similarity, liveness_score, action, "
        " floor_selected, latency_ms) "
        "VALUES (?,?,?,?,?,?,?);";
    const char* upd_touch =
        "UPDATE residents "
        "SET last_seen_at=CURRENT_TIMESTAMP, match_count=match_count+1 "
        "WHERE id=?;";

    sqlite3_stmt* st_event = nullptr;
    sqlite3_stmt* st_touch = nullptr;
    sqlite3_prepare_v2(db_, ins_event, -1, &st_event, nullptr);
    sqlite3_prepare_v2(db_, upd_touch, -1, &st_touch, nullptr);

    exec_sql(db_, "BEGIN IMMEDIATE;");
    for (const auto& j : jobs) {
        if (j.kind == WriteJob::Kind::Event && st_event) {
            const MatchEvent& e = j.event;
            sqlite3_reset(st_event);
            sqlite3_bind_int(st_event, 1, e.cabin_id);
            if (e.resident_id >= 0) sqlite3_bind_int64(st_event, 2, e.resident_id);
            else                    sqlite3_bind_null (st_event, 2);
            sqlite3_bind_double(st_event, 3, e.similarity);
            if (e.liveness_score >= 0) sqlite3_bind_double(st_event, 4, e.liveness_score);
            else                       sqlite3_bind_null  (st_event, 4);
            sqlite3_bind_text(st_event, 5, e.action.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st_event, 6, e.floor_selected);
            sqlite3_bind_int (st_event, 7, e.latency_ms);
            if (sqlite3_step(st_event) != SQLITE_DONE) {
                LOG_ERROR("db", "insert event failed: %s", sqlite3_errmsg(db_));
            }
        } else if (j.kind == WriteJob::Kind::Touch && st_touch) {
            sqlite3_reset(st_touch);
            sqlite3_bind_int64(st_touch, 1, j.resident_id);
            if (sqlite3_step(st_touch) != SQLITE_DONE) {
                LOG_ERROR("db", "touch failed: %s", sqlite3_errmsg(db_));
            }
        }
    }
    exec_sql(db_, "COMMIT;");

    if (st_event) sqlite3_finalize(st_event);
    if (st_touch) sqlite3_finalize(st_touch);
}

// --------------------------------------------------------------------------
// Phase 2 write API (synchronous — used by offline enroll tools)
// --------------------------------------------------------------------------
int64_t ResidentDB::find_resident(const std::string& name) const {
    if (!db_) return -1;
    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT id FROM residents WHERE name=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    int64_t id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}

bool ResidentDB::remove_resident(int64_t resident_id) {
    if (!db_ || resident_id < 0) return false;
    sqlite3_stmt* st = nullptr;
    const char* del = "DELETE FROM residents WHERE id=?;";
    if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, resident_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// Transaction helpers for batching synchronous writes (offline tools).
bool ResidentDB::begin()    { return db_ && exec_sql(db_, "BEGIN;"); }
bool ResidentDB::commit()   { return db_ && exec_sql(db_, "COMMIT;"); }
bool ResidentDB::rollback() { return db_ && exec_sql(db_, "ROLLBACK;"); }

int64_t ResidentDB::upsert_resident(const std::string& name, int home_floor) {
    if (!db_) return -1;

    // Try to find existing by name.
    {
        int64_t existing = find_resident(name);
        if (existing >= 0) return existing;
    }

    // Insert new.
    sqlite3_stmt* st = nullptr;
    const char* ins =
        "INSERT INTO residents (name, home_floor) VALUES (?,?);";
    if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("db", "prepare upsert failed: %s", sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, home_floor);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("db", "insert resident failed: %s", sqlite3_errmsg(db_));
        return -1;
    }
    return sqlite3_last_insert_rowid(db_);
}

bool ResidentDB::add_embedding(int64_t resident_id,
                               const std::string& source,
                               const std::vector<float>& vec) {
    if (!db_ || resident_id < 0 || vec.empty()) return false;
    sqlite3_stmt* st = nullptr;
    const char* ins =
        "INSERT INTO embeddings (resident_id, source, dim, vector) "
        "VALUES (?,?,?,?);";
    if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        LOG_ERROR("db", "prepare add_embedding failed: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(st, 1, resident_id);
    sqlite3_bind_text (st, 2, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 3, static_cast<int>(vec.size()));
    sqlite3_bind_blob (st, 4, vec.data(),
                       static_cast<int>(vec.size() * sizeof(float)),
                       SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_ERROR("db", "insert embedding failed: %s", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool ResidentDB::delete_embeddings(int64_t resident_id) {
    if (!db_ || resident_id < 0) return false;
    sqlite3_stmt* st = nullptr;
    const char* del = "DELETE FROM embeddings WHERE resident_id=?;";
    if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, resident_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// --------------------------------------------------------------------------
// Task 3.4 — close()
// --------------------------------------------------------------------------
void ResidentDB::close() {
    if (writer_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(q_mtx_);
            stop_ = true;
        }
        q_cv_.notify_all();
        writer_.join();
    }
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
