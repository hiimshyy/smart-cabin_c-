#include "traffic_db.h"

#include "log/logger.h"
#include "match_engine.h"
#include "resident_db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <utility>

namespace {

class StmtGuard {
public:
    explicit StmtGuard(sqlite3_stmt* st = nullptr) : st_(st) {}
    ~StmtGuard() {
        if (st_) sqlite3_finalize(st_);
    }
    StmtGuard(const StmtGuard&)            = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;

    sqlite3_stmt** put() {
        return &st_;
    }
    sqlite3_stmt* get() const {
        return st_;
    }
    void reset() {
        if (st_) sqlite3_reset(st_);
    }

private:
    sqlite3_stmt* st_ = nullptr;
};

bool exec_sql(sqlite3* db, const char* sql, std::string* error_out = nullptr) {
    char*     err = nullptr;
    const int rc  = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const char* msg = err ? err : sqlite3_errmsg(db);
        if (error_out) *error_out = msg ? msg : "sqlite error";
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool create_dirs_for_file(const std::string& file_path) {
    const std::size_t slash = file_path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string dir = file_path.substr(0, slash);
    std::string       cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (cur == "/") continue;
            if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

bool create_dir_recursive(const std::string& dir) {
    if (dir.empty()) return false;
    return create_dirs_for_file(dir + "/dummy");
}

void bind_opt_int(sqlite3_stmt* st, int idx, int val, bool present) {
    if (present) {
        sqlite3_bind_int(st, idx, val);
    } else {
        sqlite3_bind_null(st, idx);
    }
}

void bind_opt_ms(sqlite3_stmt* st, int idx, double ms) {
    if (ms >= 0.0 && std::isfinite(ms)) {
        sqlite3_bind_int64(st, idx, static_cast<sqlite3_int64>(std::llround(ms)));
    } else {
        sqlite3_bind_null(st, idx);
    }
}

void bind_opt_int64_nonneg(sqlite3_stmt* st, int idx, std::int64_t val) {
    if (val >= 0) {
        sqlite3_bind_int64(st, idx, static_cast<sqlite3_int64>(val));
    } else {
        sqlite3_bind_null(st, idx);
    }
}

void bind_opt_float_nonneg(sqlite3_stmt* st, int idx, float val) {
    if (val >= 0.0f && std::isfinite(val)) {
        sqlite3_bind_double(st, idx, static_cast<double>(val));
    } else {
        sqlite3_bind_null(st, idx);
    }
}

void bind_opt_text(sqlite3_stmt* st, int idx, const std::string& val) {
    if (!val.empty()) {
        sqlite3_bind_text(st, idx, val.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, idx);
    }
}

std::string col_text(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return {};
    const unsigned char* s = sqlite3_column_text(st, col);
    return s ? reinterpret_cast<const char*>(s) : std::string{};
}

std::int64_t col_int64_or(sqlite3_stmt* st, int col, std::int64_t fallback = -1) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return sqlite3_column_int64(st, col);
}

int col_int_or(sqlite3_stmt* st, int col, int fallback = -1) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return sqlite3_column_int(st, col);
}

float col_float_or(sqlite3_stmt* st, int col, float fallback = -1.0f) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return static_cast<float>(sqlite3_column_double(st, col));
}

std::string csv_escape(const std::string& field) {
    bool needs_quote = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"')
            out += "\"\"";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string col_csv(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return {};
    const unsigned char* s = sqlite3_column_text(st, col);
    return s ? csv_escape(reinterpret_cast<const char*>(s)) : std::string{};
}

std::string col_floor_label_csv(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return {};
    return csv_escape(format_floor_label(sqlite3_column_int(st, col)));
}

std::string time_window_clause(const char* col_expr, std::int64_t from_ms, std::int64_t to_ms) {
    std::ostringstream oss;
    oss << "WHERE 1=1";
    if (from_ms >= 0) {
        oss << " AND (" << col_expr << " IS NULL OR " << col_expr << " >= " << from_ms << ")";
    }
    if (to_ms >= 0) {
        oss << " AND (" << col_expr << " IS NULL OR " << col_expr << " <= " << to_ms << ")";
    }
    return oss.str();
}

} // namespace

TrafficDB::~TrafficDB() {
    close();
}

bool TrafficDB::open(const std::string& db_path, const std::string& schema_sql_path) {
    if (db_) return false;
    if (db_path.empty()) return false;
    if (db_path != ":memory:" && db_path.rfind("file:", 0) != 0) {
        create_dirs_for_file(db_path);
    }

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("traffic-db", "cannot open %s: %s", db_path.c_str(),
                  db_ ? sqlite3_errmsg(db_) : "sqlite open failed");
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    std::string sql_err;
    if (!exec_sql(db_, "PRAGMA journal_mode=WAL;", &sql_err) ||
        !exec_sql(db_, "PRAGMA foreign_keys=ON;", &sql_err) ||
        !exec_sql(db_, "PRAGMA busy_timeout=5000;", &sql_err)) {
        LOG_ERROR("traffic-db", "pragma setup failed: %s", sql_err.c_str());
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    if (!has_tables()) {
        if (!apply_schema(schema_sql_path)) {
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
    }

    if (db_path != ":memory:" && db_path.rfind("file:", 0) != 0) {
        ::chmod(db_path.c_str(), 0600);
        const std::string wal = db_path + "-wal";
        const std::string shm = db_path + "-shm";
        ::chmod(wal.c_str(), 0600);
        ::chmod(shm.c_str(), 0600);
    }
    return true;
}

void TrafficDB::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool TrafficDB::has_tables() const {
    if (!db_) return false;
    StmtGuard st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT name FROM sqlite_master WHERE type='table' AND name='app_sessions' LIMIT 1;",
            -1, st.put(), nullptr) != SQLITE_OK) {
        return false;
    }
    return sqlite3_step(st.get()) == SQLITE_ROW;
}

bool TrafficDB::apply_schema(const std::string& schema_sql_path) {
    std::ifstream in(schema_sql_path, std::ios::binary);
    if (!in) {
        LOG_ERROR("traffic-db", "cannot read schema file %s", schema_sql_path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    if (!exec_sql(db_, ss.str().c_str(), &err)) {
        LOG_ERROR("traffic-db", "apply schema failed: %s", err.c_str());
        return false;
    }
    return true;
}

bool TrafficDB::insert_session(const std::string& session_key, std::int64_t started_at_ms,
                               int cabin_id, int elevator_slave, std::string* error_out) {
    if (!db_) return false;
    StmtGuard   st;
    const char* sql =
        "INSERT OR IGNORE INTO app_sessions (session_key, started_at_ms, cabin_id, elevator_slave) "
        "VALUES (?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st.get(), 1, session_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.get(), 2, static_cast<sqlite3_int64>(started_at_ms));
    sqlite3_bind_int(st.get(), 3, cabin_id);
    sqlite3_bind_int(st.get(), 4, elevator_slave);
    if (sqlite3_step(st.get()) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool TrafficDB::insert_batch(const TrafficWriteBatch& batch, std::string* error_out) {
    if (!db_) {
        if (error_out) *error_out = "db not open";
        return false;
    }
    if (batch.stops.empty() && batch.runs.empty() && batch.run_passengers.empty() &&
        batch.trips.empty() && batch.car_calls.empty() && batch.hall_calls.empty()) {
        return true;
    }

    if (!exec_sql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    auto fail_tx = [&](const char* msg) {
        if (error_out) *error_out = msg ? msg : sqlite3_errmsg(db_);
        exec_sql(db_, "ROLLBACK;");
        return false;
    };

    if (!batch.stops.empty()) {
        StmtGuard   st;
        const char* sql = "INSERT OR IGNORE INTO cabin_stops ("
                          "stop_key, cabin_id, floor, height_cm, arrived_at_ms, door_opened_at_ms, "
                          "door_closed_at_ms, departed_at_ms, boarded, alighted, raw_8471_arrive, "
                          "raw_8472_arrive, raw_8480_arrive, raw_8471_depart, raw_8472_depart, "
                          "raw_8480_depart, flags) "
                          "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& s : batch.stops) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, s.stop_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st.get(), 2, s.cabin_id);
            bind_opt_int(st.get(), 3, s.floor, s.has_floor);
            bind_opt_int(st.get(), 4, s.height_cm, s.has_height);
            bind_opt_ms(st.get(), 5, s.arrived_at_ms);
            bind_opt_ms(st.get(), 6, s.door_opened_at_ms);
            bind_opt_ms(st.get(), 7, s.door_closed_at_ms);
            bind_opt_ms(st.get(), 8, s.departed_at_ms);
            sqlite3_bind_int(st.get(), 9, s.boarded);
            sqlite3_bind_int(st.get(), 10, s.alighted);
            bind_opt_int(st.get(), 11, s.raw_8471_arrive, s.has_8471_arrive);
            bind_opt_int(st.get(), 12, s.raw_8472_arrive, s.has_8472_arrive);
            bind_opt_int(st.get(), 13, s.raw_8480_arrive, s.has_8480_arrive);
            bind_opt_int(st.get(), 14, s.raw_8471_depart, s.has_8471_depart);
            bind_opt_int(st.get(), 15, s.raw_8472_depart, s.has_8472_depart);
            bind_opt_int(st.get(), 16, s.raw_8480_depart, s.has_8480_depart);
            sqlite3_bind_int64(st.get(), 17, static_cast<sqlite3_int64>(s.flags));
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!batch.runs.empty()) {
        StmtGuard   st;
        const char* sql =
            "INSERT OR IGNORE INTO cabin_runs ("
            "run_key, cabin_id, from_stop_key, to_stop_key, from_floor, to_floor, direction, "
            "departed_at_ms, arrived_at_ms, load_count, load_confidence, car_calls_mask, flags) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& r : batch.runs) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, r.run_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st.get(), 2, r.cabin_id);
            bind_opt_text(st.get(), 3, r.from_stop_key);
            bind_opt_text(st.get(), 4, r.to_stop_key);
            bind_opt_int(st.get(), 5, r.from_floor, r.has_from_floor);
            bind_opt_int(st.get(), 6, r.to_floor, r.has_to_floor);
            const std::string dir = r.direction.empty() ? "unknown" : r.direction;
            sqlite3_bind_text(st.get(), 7, dir.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_ms(st.get(), 8, r.departed_at_ms);
            bind_opt_ms(st.get(), 9, r.arrived_at_ms);
            bind_opt_int(st.get(), 10, r.load_count, r.has_load_count);
            bind_opt_text(st.get(), 11, r.load_confidence);
            sqlite3_bind_int(st.get(), 12, static_cast<int>(r.car_calls_mask));
            sqlite3_bind_int64(st.get(), 13, static_cast<sqlite3_int64>(r.flags));
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!batch.run_passengers.empty()) {
        StmtGuard   st;
        const char* sql = "INSERT OR IGNORE INTO run_passengers (run_key, trip_key) VALUES (?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& rp : batch.run_passengers) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, rp.run_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st.get(), 2, rp.trip_key.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!batch.trips.empty()) {
        StmtGuard   st;
        const char* sql =
            "INSERT OR IGNORE INTO passenger_trips ("
            "trip_key, cabin_id, entered_at_ms, exited_at_ms, floor_in, floor_out, "
            "identity_kind, resident_id, visitor_id, identity_sim, suggested_resident_id, "
            "suggested_resident_sim, called_floor, call_state, call_register_confirmed, "
            "car_calls_mask, rides, boarded_stop_key, alighted_stop_key, hall_call_key, "
            "car_call_key, confidence, flags, face_image) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& t : batch.trips) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, t.trip_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st.get(), 2, t.cabin_id);
            bind_opt_ms(st.get(), 3, t.entered_at_ms);
            bind_opt_ms(st.get(), 4, t.exited_at_ms);
            bind_opt_int(st.get(), 5, t.floor_in, t.has_floor_in);
            bind_opt_int(st.get(), 6, t.floor_out, t.has_floor_out);
            const std::string ikind = t.identity_kind.empty() ? "unidentified" : t.identity_kind;
            sqlite3_bind_text(st.get(), 7, ikind.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_int64_nonneg(st.get(), 8, t.resident_id);
            bind_opt_int64_nonneg(st.get(), 9, t.visitor_id);
            bind_opt_float_nonneg(st.get(), 10, t.identity_sim);
            bind_opt_int64_nonneg(st.get(), 11, t.suggested_resident_id);
            bind_opt_float_nonneg(st.get(), 12, t.suggested_resident_sim);
            bind_opt_int(st.get(), 13, t.called_floor, t.has_called_floor);
            bind_opt_text(st.get(), 14, t.call_state);
            bind_opt_int(st.get(), 15, t.call_register_confirmed, t.call_register_confirmed >= 0);
            sqlite3_bind_int(st.get(), 16, static_cast<int>(t.car_calls_mask));
            sqlite3_bind_int(st.get(), 17, t.rides);
            bind_opt_text(st.get(), 18, t.boarded_stop_key);
            bind_opt_text(st.get(), 19, t.alighted_stop_key);
            bind_opt_text(st.get(), 20, t.hall_call_key);
            bind_opt_text(st.get(), 21, t.car_call_key);
            const std::string conf = t.confidence.empty() ? "low" : t.confidence;
            sqlite3_bind_text(st.get(), 22, conf.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st.get(), 23, static_cast<sqlite3_int64>(t.flags));
            bind_opt_text(st.get(), 24, t.face_image);
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!batch.car_calls.empty()) {
        StmtGuard   st;
        const char* sql =
            "INSERT OR IGNORE INTO car_calls ("
            "call_key, cabin_id, floor, registered_at_ms, registered_stop_key, origin_floor, "
            "cleared_at_ms, served_stop_key, outcome, source, flags) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& c : batch.car_calls) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, c.call_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st.get(), 2, c.cabin_id);
            sqlite3_bind_int(st.get(), 3, c.floor);
            bind_opt_ms(st.get(), 4, c.registered_at_ms);
            bind_opt_text(st.get(), 5, c.registered_stop_key);
            bind_opt_int(st.get(), 6, c.origin_floor, c.has_origin_floor);
            bind_opt_ms(st.get(), 7, c.cleared_at_ms);
            bind_opt_text(st.get(), 8, c.served_stop_key);
            bind_opt_text(st.get(), 9, c.outcome);
            const std::string src = c.source.empty() ? "button" : c.source;
            sqlite3_bind_text(st.get(), 10, src.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st.get(), 11, static_cast<sqlite3_int64>(c.flags));
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!batch.hall_calls.empty()) {
        StmtGuard   st;
        const char* sql =
            "INSERT OR IGNORE INTO hall_calls ("
            "call_key, floor, direction, registered_at_ms, cleared_at_ms, served_cabin_id, "
            "served_stop_key, door_opened_at_ms, wait_ms, outcome, flags) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        for (const auto& h : batch.hall_calls) {
            st.reset();
            sqlite3_bind_text(st.get(), 1, h.call_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st.get(), 2, h.floor);
            sqlite3_bind_text(st.get(), 3, h.direction.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_ms(st.get(), 4, h.registered_at_ms);
            bind_opt_ms(st.get(), 5, h.cleared_at_ms);
            bind_opt_int(st.get(), 6, h.served_cabin_id, h.has_served_cabin);
            bind_opt_text(st.get(), 7, h.served_stop_key);
            bind_opt_ms(st.get(), 8, h.door_opened_at_ms);
            bind_opt_int64_nonneg(st.get(), 9, h.wait_ms);
            bind_opt_text(st.get(), 10, h.outcome);
            sqlite3_bind_int64(st.get(), 11, static_cast<sqlite3_int64>(h.flags));
            if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        }
    }

    if (!exec_sql(db_, "COMMIT;", error_out)) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool TrafficDB::delete_chunk_by_key(const char* table, const char* key_col,
                                    const char* where_clause, std::int64_t cutoff_ms,
                                    std::int64_t& total_deleted, std::string* error_out) {
    while (true) {
        if (!exec_sql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
        std::ostringstream sql;
        sql << "DELETE FROM " << table << " WHERE rowid IN (SELECT rowid FROM " << table
            << " WHERE " << where_clause << " LIMIT 500);";
        (void)key_col;
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, st.put(), nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_bind_int64(st.get(), 1, static_cast<sqlite3_int64>(cutoff_ms));
        if (sqlite3_step(st.get()) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        const int changed = sqlite3_changes(db_);
        if (!exec_sql(db_, "COMMIT;", error_out)) {
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        total_deleted += changed;
        if (changed < 500) break;
    }
    return true;
}

bool TrafficDB::purge_expired(std::int64_t now_ms, int traffic_retention_days,
                              int visitor_retention_days, TrafficPurgeResult& result,
                              std::string* error_out) {
    result = TrafficPurgeResult{};
    if (!db_) {
        if (error_out) *error_out = "db not open";
        return false;
    }

    const std::int64_t day_ms = 86400000LL;
    const std::int64_t traffic_cutoff =
        now_ms - static_cast<std::int64_t>(std::max(1, traffic_retention_days)) * day_ms;
    const std::int64_t visitor_cutoff =
        now_ms - static_cast<std::int64_t>(std::max(1, visitor_retention_days)) * day_ms;

    if (!delete_chunk_by_key("run_passengers", "run_key",
                             "run_key IN (SELECT run_key FROM cabin_runs WHERE departed_at_ms < ?)",
                             traffic_cutoff, result.rows_deleted, error_out)) {
        return false;
    }
    if (!delete_chunk_by_key("cabin_runs", "run_key",
                             "departed_at_ms IS NOT NULL AND departed_at_ms < ?", traffic_cutoff,
                             result.rows_deleted, error_out)) {
        return false;
    }
    if (!delete_chunk_by_key("cabin_stops", "stop_key",
                             "arrived_at_ms IS NOT NULL AND arrived_at_ms < ?", traffic_cutoff,
                             result.rows_deleted, error_out)) {
        return false;
    }
    if (!delete_chunk_by_key("passenger_trips", "trip_key",
                             "COALESCE(exited_at_ms, entered_at_ms) < ?", traffic_cutoff,
                             result.rows_deleted, error_out)) {
        return false;
    }
    if (!delete_chunk_by_key("car_calls", "call_key",
                             "COALESCE(cleared_at_ms, registered_at_ms) < ?", traffic_cutoff,
                             result.rows_deleted, error_out)) {
        return false;
    }
    if (!delete_chunk_by_key("hall_calls", "call_key",
                             "COALESCE(cleared_at_ms, registered_at_ms) < ?", traffic_cutoff,
                             result.rows_deleted, error_out)) {
        return false;
    }

    // Expire visitors past visitor_retention_days in batches of 500, deleting their embeddings.
    while (true) {
        if (!exec_sql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
        StmtGuard sel;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT id FROM visitors WHERE status='active' AND last_seen_ms < ? LIMIT 500;", -1,
                sel.put(), nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_bind_int64(sel.get(), 1, static_cast<sqlite3_int64>(visitor_cutoff));
        std::vector<std::int64_t> ids;
        while (sqlite3_step(sel.get()) == SQLITE_ROW) {
            ids.push_back(sqlite3_column_int64(sel.get(), 0));
        }
        if (ids.empty()) {
            exec_sql(db_, "COMMIT;");
            break;
        }

        StmtGuard upd_vis;
        StmtGuard del_emb;
        if (sqlite3_prepare_v2(db_, "UPDATE visitors SET status='expired' WHERE id=?;", -1,
                               upd_vis.put(), nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(db_, "DELETE FROM visitor_embeddings WHERE visitor_id=?;", -1,
                               del_emb.put(), nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        for (std::int64_t vid : ids) {
            upd_vis.reset();
            sqlite3_bind_int64(upd_vis.get(), 1, static_cast<sqlite3_int64>(vid));
            if (sqlite3_step(upd_vis.get()) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
            del_emb.reset();
            sqlite3_bind_int64(del_emb.get(), 1, static_cast<sqlite3_int64>(vid));
            if (sqlite3_step(del_emb.get()) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
        }
        if (!exec_sql(db_, "COMMIT;", error_out)) {
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
        result.visitors_expired += static_cast<std::int64_t>(ids.size());
        if (ids.size() < 500) break;
    }

    return true;
}

bool TrafficDB::load_active_visitors(std::vector<VisitorRecord>& out, int expected_dim) const {
    out.clear();
    if (!db_) return false;
    std::map<std::int64_t, std::size_t> idx_by_id;

    {
        StmtGuard   st;
        const char* q = "SELECT id, first_seen_ms, last_seen_ms, trip_count, status "
                        "FROM visitors WHERE status='active' ORDER BY id;";
        if (sqlite3_prepare_v2(db_, q, -1, st.put(), nullptr) != SQLITE_OK) return false;
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
            VisitorRecord rec;
            rec.id            = sqlite3_column_int64(st.get(), 0);
            rec.first_seen_ms = sqlite3_column_int64(st.get(), 1);
            rec.last_seen_ms  = sqlite3_column_int64(st.get(), 2);
            rec.trip_count    = sqlite3_column_int(st.get(), 3);
            rec.status        = col_text(st.get(), 4);
            idx_by_id[rec.id] = out.size();
            out.push_back(std::move(rec));
        }
        if (rc != SQLITE_DONE) return false;
    }

    {
        StmtGuard   st;
        const char* q = "SELECT ve.id, ve.visitor_id, ve.vector, ve.quality, ve.created_at_ms "
                        "FROM visitor_embeddings ve "
                        "JOIN visitors v ON v.id = ve.visitor_id "
                        "WHERE v.status='active' ORDER BY ve.visitor_id, ve.id;";
        if (sqlite3_prepare_v2(db_, q, -1, st.put(), nullptr) != SQLITE_OK) return false;
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
            const std::int64_t vid = sqlite3_column_int64(st.get(), 1);
            const auto         it  = idx_by_id.find(vid);
            if (it == idx_by_id.end()) continue;
            const int   nbytes = sqlite3_column_bytes(st.get(), 2);
            const void* blob   = sqlite3_column_blob(st.get(), 2);
            if (!blob || nbytes <= 0 || nbytes % static_cast<int>(sizeof(float)) != 0) continue;
            const int dim = nbytes / static_cast<int>(sizeof(float));
            if (expected_dim > 0 && dim != expected_dim) continue;
            VisitorEmbeddingData emb;
            emb.id            = sqlite3_column_int64(st.get(), 0);
            emb.visitor_id    = vid;
            emb.quality       = static_cast<float>(sqlite3_column_double(st.get(), 3));
            emb.created_at_ms = sqlite3_column_int64(st.get(), 4);
            emb.vector.resize(static_cast<std::size_t>(dim));
            std::memcpy(emb.vector.data(), blob, static_cast<std::size_t>(nbytes));
            out[it->second].embeddings.push_back(std::move(emb));
        }
        if (rc != SQLITE_DONE) return false;
    }
    return true;
}

bool TrafficDB::create_visitor(std::int64_t seen_ms, const std::vector<float>& embedding,
                               float quality, std::int64_t& new_visitor_id,
                               std::int64_t& new_embedding_id) {
    new_visitor_id   = -1;
    new_embedding_id = -1;
    if (!db_ || embedding.empty()) return false;
    if (!exec_sql(db_, "BEGIN IMMEDIATE;")) return false;

    StmtGuard ins_vis;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO visitors (first_seen_ms, last_seen_ms, trip_count, status) "
                           "VALUES (?,?,1,'active');",
                           -1, ins_vis.put(), nullptr) != SQLITE_OK) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    sqlite3_bind_int64(ins_vis.get(), 1, static_cast<sqlite3_int64>(seen_ms));
    sqlite3_bind_int64(ins_vis.get(), 2, static_cast<sqlite3_int64>(seen_ms));
    if (sqlite3_step(ins_vis.get()) != SQLITE_DONE) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    new_visitor_id = sqlite3_last_insert_rowid(db_);

    StmtGuard ins_emb;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO visitor_embeddings (visitor_id, vector, quality, "
                           "created_at_ms) VALUES (?,?,?,?);",
                           -1, ins_emb.put(), nullptr) != SQLITE_OK) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    sqlite3_bind_int64(ins_emb.get(), 1, static_cast<sqlite3_int64>(new_visitor_id));
    sqlite3_bind_blob(ins_emb.get(), 2, embedding.data(),
                      static_cast<int>(embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_double(ins_emb.get(), 3, static_cast<double>(quality));
    sqlite3_bind_int64(ins_emb.get(), 4, static_cast<sqlite3_int64>(seen_ms));
    if (sqlite3_step(ins_emb.get()) != SQLITE_DONE) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    new_embedding_id = sqlite3_last_insert_rowid(db_);

    if (!exec_sql(db_, "COMMIT;")) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool TrafficDB::record_visitor_observation(std::int64_t visitor_id, std::int64_t seen_ms,
                                           const std::vector<float>* new_embedding, float quality,
                                           std::int64_t  replace_embedding_id,
                                           std::int64_t& out_embedding_id) {
    out_embedding_id = replace_embedding_id;
    if (!db_ || visitor_id <= 0) return false;
    if (!exec_sql(db_, "BEGIN IMMEDIATE;")) return false;

    StmtGuard upd;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE visitors SET last_seen_ms = MAX(last_seen_ms, ?), trip_count = "
                           "trip_count + 1 WHERE id=?;",
                           -1, upd.put(), nullptr) != SQLITE_OK) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    sqlite3_bind_int64(upd.get(), 1, static_cast<sqlite3_int64>(seen_ms));
    sqlite3_bind_int64(upd.get(), 2, static_cast<sqlite3_int64>(visitor_id));
    if (sqlite3_step(upd.get()) != SQLITE_DONE) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }

    if (new_embedding && !new_embedding->empty()) {
        if (replace_embedding_id > 0) {
            StmtGuard upd_emb;
            if (sqlite3_prepare_v2(db_,
                                   "UPDATE visitor_embeddings SET vector=?, quality=?, "
                                   "created_at_ms=? WHERE id=? AND visitor_id=?;",
                                   -1, upd_emb.put(), nullptr) != SQLITE_OK) {
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
            sqlite3_bind_blob(upd_emb.get(), 1, new_embedding->data(),
                              static_cast<int>(new_embedding->size() * sizeof(float)),
                              SQLITE_TRANSIENT);
            sqlite3_bind_double(upd_emb.get(), 2, static_cast<double>(quality));
            sqlite3_bind_int64(upd_emb.get(), 3, static_cast<sqlite3_int64>(seen_ms));
            sqlite3_bind_int64(upd_emb.get(), 4, static_cast<sqlite3_int64>(replace_embedding_id));
            sqlite3_bind_int64(upd_emb.get(), 5, static_cast<sqlite3_int64>(visitor_id));
            if (sqlite3_step(upd_emb.get()) != SQLITE_DONE) {
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
            out_embedding_id = replace_embedding_id;
        } else {
            StmtGuard ins_emb;
            if (sqlite3_prepare_v2(db_,
                                   "INSERT INTO visitor_embeddings (visitor_id, vector, quality, "
                                   "created_at_ms) VALUES (?,?,?,?);",
                                   -1, ins_emb.put(), nullptr) != SQLITE_OK) {
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
            sqlite3_bind_int64(ins_emb.get(), 1, static_cast<sqlite3_int64>(visitor_id));
            sqlite3_bind_blob(ins_emb.get(), 2, new_embedding->data(),
                              static_cast<int>(new_embedding->size() * sizeof(float)),
                              SQLITE_TRANSIENT);
            sqlite3_bind_double(ins_emb.get(), 3, static_cast<double>(quality));
            sqlite3_bind_int64(ins_emb.get(), 4, static_cast<sqlite3_int64>(seen_ms));
            if (sqlite3_step(ins_emb.get()) != SQLITE_DONE) {
                exec_sql(db_, "ROLLBACK;");
                return false;
            }
            out_embedding_id = sqlite3_last_insert_rowid(db_);
        }
    }

    if (!exec_sql(db_, "COMMIT;")) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool TrafficDB::insert_audit(std::int64_t now_ms, const std::string& action,
                             std::int64_t visitor_id, std::int64_t resident_id,
                             const std::string& detail) {
    StmtGuard   st;
    const char* sql = "INSERT INTO admin_audit (at_ms, action, visitor_id, resident_id, detail) "
                      "VALUES (?,?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st.get(), 1, static_cast<sqlite3_int64>(now_ms));
    sqlite3_bind_text(st.get(), 2, action.c_str(), -1, SQLITE_TRANSIENT);
    bind_opt_int64_nonneg(st.get(), 3, visitor_id);
    bind_opt_int64_nonneg(st.get(), 4, resident_id);
    bind_opt_text(st.get(), 5, detail);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool TrafficDB::list_visitors(const std::string& status_filter, int limit,
                              std::vector<VisitorSummary>& out) const {
    out.clear();
    if (!db_) return false;
    const bool  filter_status = !status_filter.empty() && status_filter != "all";
    std::string sql = "SELECT v.id, v.first_seen_ms, v.last_seen_ms, v.trip_count, v.status, "
                      "       v.merged_into, v.promoted_resident_id, "
                      "       COUNT(ve.id) AS emb_cnt, COALESCE(MAX(ve.quality), 0.0) AS max_q "
                      "FROM visitors v "
                      "LEFT JOIN visitor_embeddings ve ON ve.visitor_id = v.id ";
    if (filter_status) sql += "WHERE v.status = ? ";
    sql += "GROUP BY v.id ORDER BY v.last_seen_ms DESC, v.id DESC LIMIT ?;";

    StmtGuard st;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, st.put(), nullptr) != SQLITE_OK) return false;
    int bind_idx = 1;
    if (filter_status) {
        sqlite3_bind_text(st.get(), bind_idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st.get(), bind_idx, limit > 0 ? limit : 1000000);

    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
        VisitorSummary vs;
        vs.id                   = sqlite3_column_int64(st.get(), 0);
        vs.first_seen_ms        = sqlite3_column_int64(st.get(), 1);
        vs.last_seen_ms         = sqlite3_column_int64(st.get(), 2);
        vs.trip_count           = sqlite3_column_int(st.get(), 3);
        vs.status               = col_text(st.get(), 4);
        vs.merged_into          = col_int64_or(st.get(), 5, -1);
        vs.promoted_resident_id = col_int64_or(st.get(), 6, -1);
        vs.embedding_count      = sqlite3_column_int(st.get(), 7);
        vs.max_quality          = static_cast<float>(sqlite3_column_double(st.get(), 8));
        out.push_back(std::move(vs));
    }
    return rc == SQLITE_DONE;
}

bool TrafficDB::show_visitor(std::int64_t visitor_id, VisitorShowResult& out) const {
    out = VisitorShowResult{};
    if (!db_ || visitor_id <= 0) return false;

    {
        StmtGuard   st;
        const char* sql = "SELECT v.id, v.first_seen_ms, v.last_seen_ms, v.trip_count, v.status, "
                          "       v.merged_into, v.promoted_resident_id, COUNT(ve.id), "
                          "COALESCE(MAX(ve.quality), 0.0) "
                          "FROM visitors v LEFT JOIN visitor_embeddings ve ON ve.visitor_id = v.id "
                          "WHERE v.id = ? GROUP BY v.id;";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(st.get(), 1, static_cast<sqlite3_int64>(visitor_id));
        if (sqlite3_step(st.get()) != SQLITE_ROW) return false;
        out.summary.id                   = sqlite3_column_int64(st.get(), 0);
        out.summary.first_seen_ms        = sqlite3_column_int64(st.get(), 1);
        out.summary.last_seen_ms         = sqlite3_column_int64(st.get(), 2);
        out.summary.trip_count           = sqlite3_column_int(st.get(), 3);
        out.summary.status               = col_text(st.get(), 4);
        out.summary.merged_into          = col_int64_or(st.get(), 5, -1);
        out.summary.promoted_resident_id = col_int64_or(st.get(), 6, -1);
        out.summary.embedding_count      = sqlite3_column_int(st.get(), 7);
        out.summary.max_quality          = static_cast<float>(sqlite3_column_double(st.get(), 8));
    }

    {
        StmtGuard   st;
        const char* sql = "SELECT id, visitor_id, quality, created_at_ms FROM visitor_embeddings "
                          "WHERE visitor_id = ? ORDER BY quality DESC, id DESC;";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(st.get(), 1, static_cast<sqlite3_int64>(visitor_id));
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            VisitorEmbeddingData e;
            e.id            = sqlite3_column_int64(st.get(), 0);
            e.visitor_id    = sqlite3_column_int64(st.get(), 1);
            e.quality       = static_cast<float>(sqlite3_column_double(st.get(), 2));
            e.created_at_ms = sqlite3_column_int64(st.get(), 3);
            out.embeddings.push_back(std::move(e));
        }
    }

    {
        StmtGuard   st;
        const char* sql =
            "SELECT trip_key, cabin_id, entered_at_ms, exited_at_ms, floor_in, floor_out, "
            "       identity_kind, identity_sim, suggested_resident_id, suggested_resident_sim, "
            "       confidence, flags, face_image "
            "FROM passenger_trips WHERE visitor_id = ? ORDER BY COALESCE(entered_at_ms, "
            "exited_at_ms) DESC LIMIT 25;";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(st.get(), 1, static_cast<sqlite3_int64>(visitor_id));
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            PassengerTripRecord t;
            t.trip_key               = col_text(st.get(), 0);
            t.cabin_id               = sqlite3_column_int(st.get(), 1);
            t.entered_at_ms          = sqlite3_column_type(st.get(), 2) == SQLITE_NULL
                                           ? -1.0
                                           : static_cast<double>(sqlite3_column_int64(st.get(), 2));
            t.exited_at_ms           = sqlite3_column_type(st.get(), 3) == SQLITE_NULL
                                           ? -1.0
                                           : static_cast<double>(sqlite3_column_int64(st.get(), 3));
            t.floor_in               = col_int_or(st.get(), 4, -1);
            t.has_floor_in           = t.floor_in >= 0;
            t.floor_out              = col_int_or(st.get(), 5, -1);
            t.has_floor_out          = t.floor_out >= 0;
            t.identity_kind          = col_text(st.get(), 6);
            t.visitor_id             = visitor_id;
            t.identity_sim           = col_float_or(st.get(), 7, -1.0f);
            t.suggested_resident_id  = col_int64_or(st.get(), 8, -1);
            t.suggested_resident_sim = col_float_or(st.get(), 9, -1.0f);
            t.confidence             = col_text(st.get(), 10);
            t.flags      = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 11));
            t.face_image = col_text(st.get(), 12);
            out.recent_trips.push_back(std::move(t));
        }
    }
    return true;
}

bool TrafficDB::merge_visitors(std::int64_t from_id, std::int64_t into_id, std::int64_t now_ms,
                               int max_embeddings_per_visitor, std::string& error) {
    if (!db_) {
        error = "db_not_open";
        return false;
    }
    if (from_id <= 0 || into_id <= 0 || from_id == into_id) {
        error = "invalid_visitor_ids";
        return false;
    }

    if (!exec_sql(db_, "BEGIN IMMEDIATE;", &error)) return false;
    auto fail_tx = [&](const char* msg) {
        error = msg ? msg : sqlite3_errmsg(db_);
        exec_sql(db_, "ROLLBACK;");
        return false;
    };

    std::int64_t from_first = 0, from_last = 0;
    int          from_trips = 0;
    std::string  from_status, into_status;
    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "SELECT id, first_seen_ms, last_seen_ms, trip_count, status FROM "
                               "visitors WHERE id IN (?,?);",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, from_id);
        sqlite3_bind_int64(st.get(), 2, into_id);
        bool got_from = false, got_into = false;
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            const std::int64_t vid = sqlite3_column_int64(st.get(), 0);
            if (vid == from_id) {
                got_from    = true;
                from_first  = sqlite3_column_int64(st.get(), 1);
                from_last   = sqlite3_column_int64(st.get(), 2);
                from_trips  = sqlite3_column_int(st.get(), 3);
                from_status = col_text(st.get(), 4);
            } else if (vid == into_id) {
                got_into    = true;
                into_status = col_text(st.get(), 4);
            }
        }
        if (!got_from || !got_into) return fail_tx("visitor_not_found");
        if (from_status != "active" || into_status != "active") {
            return fail_tx("visitors_must_be_active");
        }
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE visitor_embeddings SET visitor_id=? WHERE visitor_id=?;", -1,
                               st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, into_id);
        sqlite3_bind_int64(st.get(), 2, from_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    if (max_embeddings_per_visitor > 0) {
        StmtGuard   st;
        const char* sql = "DELETE FROM visitor_embeddings WHERE visitor_id=? AND id NOT IN ("
                          "  SELECT id FROM visitor_embeddings WHERE visitor_id=? ORDER BY quality "
                          "DESC, id DESC LIMIT ?"
                          ");";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, into_id);
        sqlite3_bind_int64(st.get(), 2, into_id);
        sqlite3_bind_int(st.get(), 3, max_embeddings_per_visitor);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "UPDATE passenger_trips SET visitor_id=? WHERE visitor_id=?;",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, into_id);
        sqlite3_bind_int64(st.get(), 2, from_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard   st;
        const char* sql =
            "UPDATE visitors SET first_seen_ms = MIN(first_seen_ms, ?), "
            "last_seen_ms = MAX(last_seen_ms, ?), trip_count = trip_count + ? WHERE id=?;";
        if (sqlite3_prepare_v2(db_, sql, -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, from_first);
        sqlite3_bind_int64(st.get(), 2, from_last);
        sqlite3_bind_int(st.get(), 3, from_trips);
        sqlite3_bind_int64(st.get(), 4, into_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE visitors SET status='merged', merged_into=? WHERE id=?;", -1,
                               st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, into_id);
        sqlite3_bind_int64(st.get(), 2, from_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    if (!insert_audit(now_ms, "merge", into_id, -1, "merged_from=" + std::to_string(from_id))) {
        return fail_tx(sqlite3_errmsg(db_));
    }
    if (!exec_sql(db_, "COMMIT;", &error)) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool TrafficDB::delete_visitor(std::int64_t visitor_id, std::int64_t now_ms,
                               std::vector<std::string>& face_images_to_unlink,
                               std::string&              error) {
    face_images_to_unlink.clear();
    if (!db_ || visitor_id <= 0) {
        error = "invalid_visitor_id";
        return false;
    }
    if (!exec_sql(db_, "BEGIN IMMEDIATE;", &error)) return false;
    auto fail_tx = [&](const char* msg) {
        error = msg ? msg : sqlite3_errmsg(db_);
        exec_sql(db_, "ROLLBACK;");
        return false;
    };

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "SELECT status FROM visitors WHERE id=?;", -1, st.put(),
                               nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_ROW) return fail_tx("visitor_not_found");
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "SELECT DISTINCT face_image FROM passenger_trips WHERE visitor_id=? "
                               "AND face_image IS NOT NULL;",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, visitor_id);
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            const std::string p = col_text(st.get(), 0);
            if (!p.empty()) face_images_to_unlink.push_back(p);
        }
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "DELETE FROM visitor_embeddings WHERE visitor_id=?;", -1,
                               st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE passenger_trips SET identity_kind='unidentified', "
                               "visitor_id=NULL, face_image=NULL WHERE visitor_id=?;",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "UPDATE visitors SET status='deleted' WHERE id=?;", -1,
                               st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    if (!insert_audit(now_ms, "delete", visitor_id, -1, "status=deleted")) {
        return fail_tx(sqlite3_errmsg(db_));
    }
    if (!exec_sql(db_, "COMMIT;", &error)) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

PromoteResult TrafficDB::promote_visitor(const PromoteRequest& req) {
    PromoteResult out;
    if (!db_ || req.visitor_id <= 0) {
        out.error = "invalid_visitor_id";
        return out;
    }
    if (req.resident_db_path.empty()) {
        out.error = "resident_db_path_required";
        return out;
    }
    if (req.existing_resident_id <= 0 && req.new_name.empty()) {
        out.error = "existing_resident_id_or_name_required";
        return out;
    }

    std::string status;
    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "SELECT status FROM visitors WHERE id=?;", -1, st.put(),
                               nullptr) != SQLITE_OK) {
            out.error = sqlite3_errmsg(db_);
            return out;
        }
        sqlite3_bind_int64(st.get(), 1, req.visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_ROW) {
            out.error = "visitor_not_found";
            return out;
        }
        status = col_text(st.get(), 0);
    }
    if (status != "active") {
        out.error = "visitor_not_active";
        return out;
    }

    std::vector<std::vector<float>> vecs;
    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "SELECT vector FROM visitor_embeddings WHERE visitor_id=? ORDER BY "
                               "quality DESC, id;",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            out.error = sqlite3_errmsg(db_);
            return out;
        }
        sqlite3_bind_int64(st.get(), 1, req.visitor_id);
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            const int   nbytes = sqlite3_column_bytes(st.get(), 0);
            const void* blob   = sqlite3_column_blob(st.get(), 0);
            if (!blob || nbytes <= 0 || nbytes % static_cast<int>(sizeof(float)) != 0) continue;
            std::vector<float> v(static_cast<std::size_t>(nbytes) / sizeof(float));
            std::memcpy(v.data(), blob, static_cast<std::size_t>(nbytes));
            vecs.push_back(std::move(v));
        }
    }
    if (vecs.empty()) {
        out.error = "visitor_has_no_embeddings";
        return out;
    }

    ResidentDB rdb;
    if (!rdb.open(req.resident_db_path, req.resident_schema_path, /*spawn_writer=*/false)) {
        out.error = "cannot_open_resident_db";
        return out;
    }

    std::int64_t target_resident_id = req.existing_resident_id;
    if (target_resident_id > 0) {
        std::vector<Resident>     residents;
        std::vector<EmbeddingRow> embeddings;
        if (!rdb.load_active(residents, embeddings)) {
            out.error = "load_active_residents_failed";
            return out;
        }
        bool found = false;
        for (const auto& r : residents) {
            if (r.id == target_resident_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            out.error = "target_resident_not_found";
            return out;
        }
    }

    if (!rdb.begin()) {
        out.error = "resident_db_begin_failed";
        return out;
    }

    if (target_resident_id <= 0) {
        target_resident_id = rdb.upsert_resident(req.new_name, req.home_floor);
        if (target_resident_id <= 0) {
            rdb.rollback();
            out.error = "create_resident_failed";
            return out;
        }
    }

    int copied = 0;
    for (const auto& v : vecs) {
        if (!rdb.add_embedding(target_resident_id, "cabin", v)) {
            rdb.rollback();
            out.error = "copy_embedding_failed";
            return out;
        }
        ++copied;
    }
    if (!rdb.commit()) {
        rdb.rollback();
        out.error = "resident_db_commit_failed";
        return out;
    }
    rdb.close();

    // Update TrafficDB inside a transaction.
    if (!exec_sql(db_, "BEGIN IMMEDIATE;", &out.error)) return out;
    auto fail_tx = [&](const char* msg) {
        out.error = msg ? msg : sqlite3_errmsg(db_);
        exec_sql(db_, "ROLLBACK;");
        return out;
    };

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(
                db_, "UPDATE visitors SET status='promoted', promoted_resident_id=? WHERE id=?;",
                -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, target_resident_id);
        sqlite3_bind_int64(st.get(), 2, req.visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE passenger_trips SET identity_kind='resident', "
                               "resident_id=?, visitor_id=NULL WHERE visitor_id=?;",
                               -1, st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, target_resident_id);
        sqlite3_bind_int64(st.get(), 2, req.visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
        out.trips_updated = sqlite3_changes(db_);
    }

    {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, "DELETE FROM visitor_embeddings WHERE visitor_id=?;", -1,
                               st.put(), nullptr) != SQLITE_OK) {
            return fail_tx(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st.get(), 1, req.visitor_id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) return fail_tx(sqlite3_errmsg(db_));
    }

    if (!insert_audit(req.now_ms, "promote", req.visitor_id, target_resident_id,
                      "embeddings=" + std::to_string(copied))) {
        return fail_tx(sqlite3_errmsg(db_));
    }
    if (!exec_sql(db_, "COMMIT;", &out.error)) {
        exec_sql(db_, "ROLLBACK;");
        return out;
    }

    out.ok                = true;
    out.resident_id       = target_resident_id;
    out.embeddings_copied = copied;
    return out;
}

bool TrafficDB::suggest_promotions(const std::string& resident_db_path, int recog_dim,
                                   int min_trips, float min_sim,
                                   std::vector<PromotionSuggestion>& out,
                                   std::string&                      error) const {
    out.clear();
    if (!db_) {
        error = "db_not_open";
        return false;
    }

    ResidentDB rdb;
    if (!rdb.open_readonly(resident_db_path, 500)) {
        error = "cannot_open_resident_db";
        return false;
    }
    std::vector<Resident>     residents;
    std::vector<EmbeddingRow> resident_embs;
    if (!rdb.load_active_snapshot(residents, resident_embs)) {
        error = "load_active_residents_failed";
        return false;
    }
    rdb.close();

    std::map<std::int64_t, Resident> resident_by_id;
    for (const auto& r : residents)
        resident_by_id[r.id] = r;

    MatchEngine engine;
    engine.build(resident_embs, recog_dim);
    if (engine.empty()) return true;

    std::vector<VisitorRecord> visitors;
    if (!load_active_visitors(visitors, recog_dim)) {
        error = "load_active_visitors_failed";
        return false;
    }

    for (const auto& v : visitors) {
        if (v.trip_count < min_trips || v.embeddings.empty()) continue;
        std::int64_t best_rid = -1;
        float        best_sim = -1.0f;
        for (const auto& emb : v.embeddings) {
            const MatchResult mr = engine.match(emb.vector, min_sim);
            if (mr.resident_id >= 0 && mr.similarity > best_sim) {
                best_rid = mr.resident_id;
                best_sim = mr.similarity;
            }
        }
        if (best_rid >= 0) {
            PromotionSuggestion s;
            s.visitor_id     = v.id;
            s.resident_id    = best_rid;
            s.max_similarity = best_sim;
            s.trip_count     = v.trip_count;
            const auto rit   = resident_by_id.find(best_rid);
            if (rit != resident_by_id.end()) {
                s.resident_name = rit->second.name;
                s.home_floor    = rit->second.home_floor;
            }
            out.push_back(std::move(s));
        }
    }

    std::sort(out.begin(), out.end(),
              [](const PromotionSuggestion& a, const PromotionSuggestion& b) {
                  return a.max_similarity > b.max_similarity;
              });
    return true;
}

TrafficExportResult TrafficDB::export_csv(const std::string& out_dir, std::int64_t from_ms,
                                          std::int64_t to_ms) const {
    TrafficExportResult res;
    if (!db_) {
        res.error = "db_not_open";
        return res;
    }
    if (!create_dir_recursive(out_dir)) {
        res.error = "cannot_create_out_dir";
        return res;
    }

    auto write_query = [&](const std::string& filename, const std::string& header,
                           const std::string& sql, const std::vector<int>& floor_cols,
                           const std::string& table_key) -> bool {
        StmtGuard st;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, st.put(), nullptr) != SQLITE_OK) {
            res.error = sqlite3_errmsg(db_);
            return false;
        }
        const std::string full_path = out_dir + "/" + filename;
        std::ofstream     out(full_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            res.error = "cannot_open_output_file: " + full_path;
            return false;
        }
        out << header << "\n";
        const int ncols = sqlite3_column_count(st.get());
        int       rows  = 0;
        int       rc    = SQLITE_ROW;
        while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
            for (int c = 0; c < ncols; ++c) {
                if (c > 0) out << ',';
                out << col_csv(st.get(), c);
            }
            for (int fc : floor_cols) {
                out << ',' << col_floor_label_csv(st.get(), fc);
            }
            out << '\n';
            ++rows;
        }
        if (rc != SQLITE_DONE) {
            res.error = sqlite3_errmsg(db_);
            return false;
        }
        res.row_counts[table_key] = rows;
        return true;
    };

    const std::string stop_where = time_window_clause("arrived_at_ms", from_ms, to_ms);
    if (!write_query("cabin_stops.csv",
                     "stop_key,cabin_id,floor,height_cm,arrived_at_ms,door_opened_at_ms,"
                     "door_closed_at_ms,departed_at_ms,boarded,alighted,raw_8471_arrive,"
                     "raw_8472_arrive,raw_8480_arrive,raw_8471_depart,raw_8472_depart,"
                     "raw_8480_depart,flags,floor_label",
                     "SELECT stop_key,cabin_id,floor,height_cm,arrived_at_ms,door_opened_at_ms,"
                     "door_closed_at_ms,departed_at_ms,boarded,alighted,raw_8471_arrive,"
                     "raw_8472_arrive,raw_8480_arrive,raw_8471_depart,raw_8472_depart,"
                     "raw_8480_depart,flags FROM cabin_stops " +
                         stop_where + " ORDER BY arrived_at_ms, stop_key;",
                     {2}, "cabin_stops")) {
        return res;
    }

    const std::string run_where = time_window_clause("departed_at_ms", from_ms, to_ms);
    if (!write_query("cabin_runs.csv",
                     "run_key,cabin_id,from_stop_key,to_stop_key,from_floor,to_floor,direction,"
                     "departed_at_ms,arrived_at_ms,load_count,load_confidence,car_calls_mask,"
                     "flags,from_floor_label,to_floor_label",
                     "SELECT run_key,cabin_id,from_stop_key,to_stop_key,from_floor,to_floor,"
                     "direction,departed_at_ms,arrived_at_ms,load_count,load_confidence,"
                     "car_calls_mask,flags FROM cabin_runs " +
                         run_where + " ORDER BY departed_at_ms, run_key;",
                     {4, 5}, "cabin_runs")) {
        return res;
    }

    const std::string trip_where =
        time_window_clause("COALESCE(entered_at_ms, exited_at_ms)", from_ms, to_ms);
    if (!write_query(
            "passenger_trips.csv",
            "trip_key,cabin_id,entered_at_ms,exited_at_ms,floor_in,floor_out,identity_kind,"
            "resident_id,visitor_id,identity_sim,suggested_resident_id,suggested_resident_sim,"
            "called_floor,call_state,call_register_confirmed,car_calls_mask,rides,"
            "boarded_stop_key,alighted_stop_key,hall_call_key,car_call_key,confidence,flags,"
            "face_image,floor_in_label,floor_out_label,called_floor_label",
            "SELECT trip_key,cabin_id,entered_at_ms,exited_at_ms,floor_in,floor_out,"
            "identity_kind,resident_id,visitor_id,identity_sim,suggested_resident_id,"
            "suggested_resident_sim,called_floor,call_state,call_register_confirmed,"
            "car_calls_mask,rides,boarded_stop_key,alighted_stop_key,hall_call_key,"
            "car_call_key,confidence,flags,face_image FROM passenger_trips " +
                trip_where + " ORDER BY COALESCE(entered_at_ms, exited_at_ms), trip_key;",
            {4, 5, 12}, "passenger_trips")) {
        return res;
    }

    const std::string cc_where = time_window_clause("registered_at_ms", from_ms, to_ms);
    if (!write_query(
            "car_calls.csv",
            "call_key,cabin_id,floor,registered_at_ms,registered_stop_key,origin_floor,"
            "cleared_at_ms,served_stop_key,outcome,source,flags,origin_floor_label,floor_label",
            "SELECT call_key,cabin_id,floor,registered_at_ms,registered_stop_key,origin_floor,"
            "cleared_at_ms,served_stop_key,outcome,source,flags FROM car_calls " +
                cc_where + " ORDER BY registered_at_ms, call_key;",
            {5, 2}, "car_calls")) {
        return res;
    }

    const std::string hc_where = time_window_clause("registered_at_ms", from_ms, to_ms);
    if (!write_query(
            "hall_calls.csv",
            "call_key,floor,direction,registered_at_ms,cleared_at_ms,served_cabin_id,"
            "served_stop_key,door_opened_at_ms,wait_ms,outcome,flags,floor_label",
            "SELECT call_key,floor,direction,registered_at_ms,cleared_at_ms,served_cabin_id,"
            "served_stop_key,door_opened_at_ms,wait_ms,outcome,flags FROM hall_calls " +
                hc_where + " ORDER BY registered_at_ms, call_key;",
            {1}, "hall_calls")) {
        return res;
    }

    if (!write_query("visitors.csv",
                     "id,first_seen_ms,last_seen_ms,trip_count,status,merged_into,"
                     "promoted_resident_id",
                     "SELECT id,first_seen_ms,last_seen_ms,trip_count,status,merged_into,"
                     "promoted_resident_id FROM visitors ORDER BY id;",
                     {}, "visitors")) {
        return res;
    }

    res.ok = true;
    return res;
}
