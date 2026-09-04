#include "resident_db.h"

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
        std::fprintf(stderr, "[residentdb] SQL error: %s\n",
                     err ? err : sqlite3_errmsg(db));
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
                      const std::string& schema_sql_path) {
    if (db_) {
        std::fprintf(stderr, "[residentdb] already open\n");
        return false;
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[residentdb] cannot open %s: %s\n",
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
            std::fprintf(stderr,
                "[residentdb] failed to apply schema from %s\n",
                schema_sql_path.c_str());
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
        std::fprintf(stderr, "[residentdb] applied schema from %s\n",
                     schema_sql_path.c_str());
    }

    // Start background writer.
    stop_ = false;
    writer_ = std::thread(&ResidentDB::writer_loop, this);
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
        std::fprintf(stderr, "[residentdb] cannot read schema file %s\n",
                     schema_sql_path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string sql = ss.str();
    return exec_sql(db_, sql.c_str());
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
            std::fprintf(stderr, "[residentdb] prepare residents failed: %s\n",
                         sqlite3_errmsg(db_));
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
            std::fprintf(stderr, "[residentdb] prepare embeddings failed: %s\n",
                         sqlite3_errmsg(db_));
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
                std::fprintf(stderr, "[residentdb] insert event failed: %s\n",
                             sqlite3_errmsg(db_));
            }
        } else if (j.kind == WriteJob::Kind::Touch && st_touch) {
            sqlite3_reset(st_touch);
            sqlite3_bind_int64(st_touch, 1, j.resident_id);
            if (sqlite3_step(st_touch) != SQLITE_DONE) {
                std::fprintf(stderr, "[residentdb] touch failed: %s\n",
                             sqlite3_errmsg(db_));
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
int64_t ResidentDB::upsert_resident(const std::string& name, int home_floor) {
    if (!db_) return -1;

    // Try to find existing by name.
    {
        sqlite3_stmt* st = nullptr;
        const char* q = "SELECT id FROM residents WHERE name=? LIMIT 1;";
        if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
                return id;
            }
            sqlite3_finalize(st);
        }
    }

    // Insert new.
    sqlite3_stmt* st = nullptr;
    const char* ins =
        "INSERT INTO residents (name, home_floor) VALUES (?,?);";
    if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "[residentdb] prepare upsert failed: %s\n",
                     sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, home_floor);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        std::fprintf(stderr, "[residentdb] insert resident failed: %s\n",
                     sqlite3_errmsg(db_));
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
        std::fprintf(stderr, "[residentdb] prepare add_embedding failed: %s\n",
                     sqlite3_errmsg(db_));
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
        std::fprintf(stderr, "[residentdb] insert embedding failed: %s\n",
                     sqlite3_errmsg(db_));
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
