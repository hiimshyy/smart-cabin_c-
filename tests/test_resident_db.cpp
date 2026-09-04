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

#define CHECK(cond, msg)                                              \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                              \
            ++g_fail;                                               \
            std::printf("  FAIL: %s  (line %d)\n", msg, __LINE__);  \
        }                                                          \
    } while (0)

// Query a single integer via a raw sqlite handle (for verifying async writes).
static long long query_ll(const std::string& db_path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return -999;
    sqlite3_stmt* st = nullptr;
    long long v = -999;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

static std::vector<float> make_vec(int dim, float seed) {
    std::vector<float> v(dim);
    double s = 0;
    for (int i = 0; i < dim; ++i) { v[i] = seed + i * 0.01f; s += (double)v[i]*v[i]; }
    double inv = 1.0 / std::sqrt(s);
    for (auto& x : v) x = (float)(x * inv);
    return v;
}

int main(int argc, char** argv) {
    const std::string schema = (argc > 1) ? argv[1] : "db/schema.sql";
    const std::string db_path = "/tmp/scabin_test.db";

    // Fresh start.
    std::remove(db_path.c_str());
    std::remove((db_path + "-wal").c_str());
    std::remove((db_path + "-shm").c_str());

    const int dim = 8;

    // ---- open() applies schema on empty DB ----
    {
        ResidentDB db;
        bool ok = db.open(db_path, schema);
        CHECK(ok, "open() succeeds and applies schema");
        CHECK(db.is_open(), "is_open() true after open");
        db.close();
        CHECK(!db.is_open(), "is_open() false after close");
    }

    // Schema tables should exist (default cabin row inserted by schema).
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM residents;") == 0,
          "residents table empty initially");
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM cabins;") >= 1,
          "cabins has default row");

    // ---- upsert_resident + add_embedding + load_active round-trip ----
    {
        ResidentDB db;
        CHECK(db.open(db_path, schema), "re-open existing DB (no re-apply)");

        int64_t alice = db.upsert_resident("Alice", 12);
        int64_t bob   = db.upsert_resident("Bob");  // home_floor defaults to 0
        CHECK(alice > 0 && bob > 0 && alice != bob, "two distinct resident ids");

        // upsert same name returns same id (no duplicate).
        int64_t alice2 = db.upsert_resident("Alice", 12);
        CHECK(alice2 == alice, "upsert existing name returns same id");

        CHECK(db.add_embedding(alice, "id_photo", make_vec(dim, 1.0f)),
              "add embedding 1 for Alice");
        CHECK(db.add_embedding(alice, "cabin", make_vec(dim, 2.0f)),
              "add embedding 2 for Alice");
        CHECK(db.add_embedding(bob, "id_photo", make_vec(dim, 3.0f)),
              "add embedding for Bob");

        std::vector<Resident>   residents;
        std::vector<EmbeddingRow> embs;
        CHECK(db.load_active(residents, embs), "load_active succeeds");
        CHECK(residents.size() == 2, "loaded 2 active residents");
        CHECK(embs.size() == 3, "loaded 3 embeddings");

        // Verify Bob's home_floor is the 0 sentinel.
        int bob_floor = -1;
        for (auto& r : residents) if (r.name == "Bob") bob_floor = r.home_floor;
        CHECK(bob_floor == HOME_FLOOR_UNSET, "Bob home_floor == 0 sentinel");

        // Verify embedding vectors round-trip with correct dim.
        bool dim_ok = true;
        for (auto& e : embs) if ((int)e.vector.size() != dim) dim_ok = false;
        CHECK(dim_ok, "all embeddings have dim 8 after round-trip");

        // ---- delete_embeddings ----
        CHECK(db.delete_embeddings(alice), "delete Alice embeddings");
        db.load_active(residents, embs);
        CHECK(embs.size() == 1, "1 embedding left after deleting Alice's");

        // ---- async log_event + touch_resident ----
        MatchEvent ev;
        ev.cabin_id = 1;
        ev.resident_id = bob;
        ev.similarity = 0.82f;
        ev.action = "matched";
        ev.floor_selected = 5;
        ev.latency_ms = 42;
        db.log_event(ev);

        MatchEvent ev2;               // unknown event (resident_id < 0 -> NULL)
        ev2.cabin_id = 1;
        ev2.resident_id = -1;
        ev2.similarity = 0.10f;
        ev2.action = "unknown";
        db.log_event(ev2);

        db.touch_resident(bob);

        // close() flushes the writer thread -> data must be committed.
        db.close();
    }

    // ---- verify async writes landed ----
    CHECK(query_ll(db_path, "SELECT COUNT(*) FROM match_events;") == 2,
          "2 match_events written");
    CHECK(query_ll(db_path,
            "SELECT COUNT(*) FROM match_events WHERE resident_id IS NULL;") == 1,
          "1 event has NULL resident_id (unknown)");
    CHECK(query_ll(db_path,
            "SELECT match_count FROM residents WHERE name='Bob';") == 1,
          "Bob match_count incremented to 1");
    CHECK(query_ll(db_path,
            "SELECT floor_selected FROM match_events WHERE action='matched';") == 5,
          "matched event floor_selected == 5");

    std::printf("\n[test_resident_db] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
