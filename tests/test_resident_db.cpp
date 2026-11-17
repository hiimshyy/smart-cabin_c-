// Unit test for ResidentDB (spec resident-db-layer, Task 3.5).
// Needs libsqlite3-dev. Uses db/schema.sql for the schema. Works on the
// dev machine (no NPU). Build via tests/run_tests.sh.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include "resident_db.h"

static int g_checks = 0;
static int g_fail   = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fail;                                                                              \
            std::printf("  FAIL: %s  (line %d)\n", msg, __LINE__);                                 \
        }                                                                                          \
    } while (0)

// Query a single integer via a raw sqlite handle (for verifying async writes).
static long long query_ll(const std::string& db_path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return -999;
    sqlite3_stmt* st = nullptr;
    long long     v  = -999;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

static std::vector<float> make_vec(int dim, float seed) {
    std::vector<float> v(dim);
    double             s = 0;
    for (int i = 0; i < dim; ++i) {
        v[i] = seed + i * 0.01f;
        s += (double)v[i] * v[i];
    }
    double inv = 1.0 / std::sqrt(s);
    for (auto& x : v)
        x = (float)(x * inv);
    return v;
}

int main(int argc, char** argv) {
    const std::string schema  = (argc > 1) ? argv[1] : "db/schema.sql";
    const std::string db_path = "/tmp/scabin_test.db";

    // Fresh start.
    std::remove(db_path.c_str());
    std::remove((db_path + "-wal").c_str());
    std::remove((db_path + "-shm").c_str());

    const int dim = 8;

    // ---- open() applies schema on empty DB ----
    {
        ResidentDB db;
        bool       ok = db.open(db_path, schema);
        CHECK(ok, "open() succeeds and applies schema");
        CHECK(db.is_open(), "is_open() true after open");
        db.close();
        CHECK(!db.is_open(), "is_open() false after close");
    }

    // Schema tables should exist (default cabin row inserted by schema).
    CHECK(HOME_FLOOR_UNSET == -1, "HOME_FLOOR_UNSET == -1");
    CHECK(SECONDARY_FLOOR_UNSET == -1, "SECONDARY_FLOOR_UNSET == -1");
    {
        int parsed = -999;
        CHECK(parse_floor_value("B", parsed) && parsed == 0, "parse_floor_value(\"B\") == 0");
        CHECK(parse_floor_value("b", parsed) && parsed == 0, "parse_floor_value(\"b\") == 0");
        CHECK(parse_floor_value("", parsed) && parsed == -1, "parse_floor_value(\"\") == -1");
        CHECK(parse_floor_value("-1", parsed) && parsed == -1, "parse_floor_value(\"-1\") == -1");
        CHECK(parse_floor_value("5", parsed) && parsed == 5, "parse_floor_value(\"5\") == 5");
        CHECK(!parse_floor_value("abc", parsed), "parse_floor_value rejects non-numeric");
        CHECK(!parse_floor_value("63", parsed), "parse_floor_value rejects > 62");
    }
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM residents;") == 0,
          "residents table empty initially");
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM cabins;") >= 1, "cabins has default row");
    CHECK(query_ll(db_path, "SELECT MAX(version) FROM schema_version;") == 4,
          "fresh schema is version 4");
    CHECK(query_ll(db_path, "SELECT floors_min FROM cabins WHERE id=1;") == 0,
          "default cabin floors_min is 0 (Basement B)");

    // ---- upsert_resident + add_embedding + load_active round-trip ----
    {
        ResidentDB db;
        CHECK(db.open(db_path, schema), "re-open existing DB (no re-apply)");

        int64_t alice = db.upsert_resident("Alice", 12, 1);
        int64_t bob   = db.upsert_resident("Bob"); // home_floor/secondary_floor default to -1
        CHECK(alice > 0 && bob > 0 && alice != bob, "two distinct resident ids");

        // upsert same name returns same id (no duplicate).
        int64_t alice2 = db.upsert_resident("Alice", 12, 1);
        CHECK(alice2 == alice, "upsert existing name returns same id");

        CHECK(db.add_embedding(alice, "id_photo", make_vec(dim, 1.0f)),
              "add embedding 1 for Alice");
        CHECK(db.add_embedding(alice, "cabin", make_vec(dim, 2.0f)), "add embedding 2 for Alice");
        CHECK(db.add_embedding(bob, "id_photo", make_vec(dim, 3.0f)), "add embedding for Bob");

        std::vector<Resident>     residents;
        std::vector<EmbeddingRow> embs;
        CHECK(db.load_active(residents, embs), "load_active succeeds");
        CHECK(residents.size() == 2, "loaded 2 active residents");
        CHECK(embs.size() == 3, "loaded 3 embeddings");

        // Verify Alice's and Bob's home_floor / secondary_floor sentinels.
        int bob_floor = -999, bob_sec = -999, alice_floor = -999, alice_sec = -999;
        for (auto& r : residents) {
            if (r.name == "Bob") {
                bob_floor = r.home_floor;
                bob_sec   = r.secondary_floor;
            }
            if (r.name == "Alice") {
                alice_floor = r.home_floor;
                alice_sec   = r.secondary_floor;
            }
        }
        CHECK(bob_floor == HOME_FLOOR_UNSET, "Bob home_floor == -1 sentinel");
        CHECK(bob_sec == SECONDARY_FLOOR_UNSET, "Bob secondary_floor == -1 sentinel");
        CHECK(alice_floor == 12 && alice_sec == 1, "Alice home_floor=12, secondary_floor=1");

        int64_t basement_id = db.upsert_resident_by_ext_id("EXT-B", "Charlie", 3, "manager", 0);
        CHECK(basement_id > 0, "upsert_resident_by_ext_id inserts secondary_floor=0 (B)");
        CHECK(db.load_active(residents, embs), "load_active after Charlie");
        int charlie_sec = -999;
        for (auto& r : residents)
            if (r.ext_id == "EXT-B") charlie_sec = r.secondary_floor;
        CHECK(charlie_sec == 0, "secondary_floor=0 (Basement B) round-trips");

        // Verify embedding vectors round-trip with correct dim.
        bool dim_ok = true;
        for (auto& e : embs)
            if ((int)e.vector.size() != dim) dim_ok = false;
        CHECK(dim_ok, "all embeddings have dim 8 after round-trip");

        // ---- delete_embeddings ----
        CHECK(db.delete_embeddings(alice), "delete Alice embeddings");
        db.load_active(residents, embs);
        CHECK(embs.size() == 1, "1 embedding left after deleting Alice's");

        // ---- async log_event + touch_resident ----
        MatchEvent ev;
        ev.cabin_id       = 1;
        ev.resident_id    = bob;
        ev.similarity     = 0.82f;
        ev.action         = "matched";
        ev.floor_selected = 5;
        ev.latency_ms     = 42;
        db.log_event(ev);

        MatchEvent ev2; // unknown event (resident_id < 0 -> NULL)
        ev2.cabin_id    = 1;
        ev2.resident_id = -1;
        ev2.similarity  = 0.10f;
        ev2.action      = "unknown";
        db.log_event(ev2);

        db.touch_resident(bob);

        // close() flushes the writer thread -> data must be committed.
        db.close();
    }

    // ---- verify async writes landed ----
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM match_events;") == 2, "2 match_events written");
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM match_events WHERE resident_id IS NULL;") == 1,
          "1 event has NULL resident_id (unknown)");
    CHECK(query_ll(db_path, "SELECT match_count FROM residents WHERE name='Bob';") == 1,
          "Bob match_count incremented to 1");
    CHECK(query_ll(db_path, "SELECT floor_selected FROM match_events WHERE action='matched';") == 5,
          "matched event floor_selected == 5");

    // ---- verify migration v3 -> v4 on an existing v3 DB ----
    {
        const std::string v3_path = "/tmp/scabin_test_v3.db";
        std::remove(v3_path.c_str());
        std::remove((v3_path + "-wal").c_str());
        std::remove((v3_path + "-shm").c_str());
        sqlite3* raw = nullptr;
        CHECK(sqlite3_open(v3_path.c_str(), &raw) == SQLITE_OK, "create v3 DB");
        const char* v3_sql =
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TIMESTAMP "
            "DEFAULT CURRENT_TIMESTAMP);"
            "INSERT INTO schema_version(version) VALUES (1),(2),(3);"
            "CREATE TABLE residents (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
            "apartment TEXT, home_floor INTEGER NOT NULL, language TEXT NOT NULL DEFAULT 'vi', "
            "greeting_name TEXT, role TEXT NOT NULL DEFAULT 'staff', active INTEGER NOT NULL "
            "DEFAULT 1, consent_at TIMESTAMP, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, last_seen_at TIMESTAMP, match_count "
            "INTEGER DEFAULT 0, notes TEXT, ext_id TEXT);"
            "CREATE TABLE embeddings (id INTEGER PRIMARY KEY AUTOINCREMENT, resident_id INTEGER "
            "NOT NULL, source TEXT NOT NULL, dim INTEGER NOT NULL, vector BLOB NOT NULL, "
            "quality_score REAL, cabin_id INTEGER, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);"
            "CREATE TABLE cabins (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
            "location TEXT, camera_urls TEXT NOT NULL, elevator_endpoint TEXT, floors_min INTEGER "
            "NOT NULL DEFAULT 1, floors_max INTEGER NOT NULL DEFAULT 30, gst_latency_ms INTEGER "
            "NOT NULL DEFAULT 100, match_thr REAL NOT NULL DEFAULT 0.35, confirm_streak INTEGER "
            "NOT NULL DEFAULT 5, cooldown_ms INTEGER NOT NULL DEFAULT 3000, unknown_after_ms "
            "INTEGER NOT NULL DEFAULT 2000, reconnect_min_ms INTEGER NOT NULL DEFAULT 500, "
            "reconnect_max_ms INTEGER NOT NULL DEFAULT 10000, created_at TIMESTAMP DEFAULT "
            "CURRENT_TIMESTAMP);"
            "INSERT INTO cabins(id,name,camera_urls,floors_min,floors_max) VALUES "
            "(1,'C1','[]',1,9);"
            "INSERT INTO residents(name,home_floor,role,ext_id) VALUES ('Legacy',5,'staff','L1');";
        char* err = nullptr;
        CHECK(sqlite3_exec(raw, v3_sql, nullptr, nullptr, &err) == SQLITE_OK, "seed v3 schema");
        if (err) sqlite3_free(err);
        sqlite3_close(raw);

        ResidentDB migrated;
        CHECK(migrated.open(v3_path, schema, false), "migrate v3 DB to v4");
        std::vector<Resident>     mr;
        std::vector<EmbeddingRow> me;
        CHECK(migrated.load_active(mr, me) && mr.size() == 1 && mr[0].secondary_floor == -1,
              "migrated resident gets secondary_floor == -1");
        migrated.close();
        CHECK(query_ll(v3_path, "SELECT MAX(version) FROM schema_version;") == 4,
              "v3 DB upgraded to schema_version 4");
        CHECK(query_ll(v3_path, "SELECT floors_min FROM cabins WHERE id=1;") == 0,
              "v3 cabin floors_min migrated from 1 to 0");
        std::remove(v3_path.c_str());
        std::remove((v3_path + "-wal").c_str());
        std::remove((v3_path + "-shm").c_str());
    }

    std::printf("\n[test_resident_db] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
