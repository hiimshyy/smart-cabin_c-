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

class ResidentDB {
public:
    ResidentDB() = default;
    ~ResidentDB();

    ResidentDB(const ResidentDB&)            = delete;
    ResidentDB& operator=(const ResidentDB&) = delete;

    // Open (or create) the DB. WAL + foreign_keys ON. If the DB has no
    // tables yet, apply the schema from `schema_sql_path`. Starts the
    // background writer thread. Returns false on any failure.
    bool open(const std::string& db_path,
              const std::string& schema_sql_path = "db/schema.sql");

    // Load all active (active=1) residents plus their embeddings.
    bool load_active(std::vector<Resident>& residents,
                     std::vector<EmbeddingRow>& embeddings);

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

    sqlite3* db_ = nullptr;

    std::thread             writer_;
    std::mutex              q_mtx_;
    std::condition_variable q_cv_;
    std::vector<WriteJob>   queue_;
    bool                    stop_ = false;
};
