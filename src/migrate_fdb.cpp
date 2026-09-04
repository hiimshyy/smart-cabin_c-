// migrate_fdb — one-way importer: legacy .fdb  ->  SQLite resident DB.
// (spec resident-db-layer, Task 4 / R3)
//
// The old .fdb format stores ONE averaged embedding per identity. This tool
// reads each identity and writes it into the SQLite schema as:
//   - one row in `residents` (name only; home_floor = 0 sentinel = "no floor
//     registered yet", spec req §6), and
//   - one row in `embeddings` (source='id_photo', the averaged vector).
//
// Duplicate handling (matched by resident name):
//   - default : SKIP the identity and log it.
//   - --overwrite : delete the resident's existing embeddings, then re-add.
//
// This tool needs NO NPU / AI SDK — only FaceDB (pure I/O) + ResidentDB
// (sqlite3). It therefore builds and runs on a plain dev machine.
//
// Usage:
//   ./migrate_fdb --fdb db/faces_all.fdb --db db/residents.db [--overwrite]
//                 [--schema db/schema.sql]

#include <cstdio>
#include <cstring>
#include <string>

#include "face_db.h"
#include "resident_db.h"

static void usage(const char* argv0) {
    std::printf(
        "Usage: %s --fdb <path.fdb> --db <sqlite.db> [--overwrite] "
        "[--schema db/schema.sql]\n"
        "\n"
        "  --fdb PATH       source legacy .fdb file (required)\n"
        "  --db PATH        destination SQLite database (required)\n"
        "  --overwrite      replace embeddings of residents that already exist\n"
        "                   (default: skip existing residents)\n"
        "  --schema PATH    schema.sql applied when the DB is empty "
        "(default db/schema.sql)\n",
        argv0);
}

int main(int argc, char** argv) {
    std::string fdb_path;
    std::string db_path;
    std::string schema_path = "db/schema.sql";
    bool overwrite = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fdb") && i + 1 < argc) {
            fdb_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--db") && i + 1 < argc) {
            db_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--schema") && i + 1 < argc) {
            schema_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--overwrite")) {
            overwrite = true;
        } else if (!std::strcmp(argv[i], "-h") ||
                   !std::strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown/incomplete arg: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (fdb_path.empty() || db_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    // ---- 1. Load the source .fdb ------------------------------------
    FaceDB fdb;
    if (!fdb.load(fdb_path)) {
        std::fprintf(stderr, "[migrate] failed to load .fdb: %s\n",
                     fdb_path.c_str());
        return 1;
    }
    std::printf("[migrate] loaded %zu identities (dim=%d) from %s\n",
                fdb.size(), fdb.dim(), fdb_path.c_str());

    // ---- 2. Open the destination SQLite DB --------------------------
    ResidentDB db;
    if (!db.open(db_path, schema_path)) {
        std::fprintf(stderr, "[migrate] failed to open SQLite DB: %s\n",
                     db_path.c_str());
        return 1;
    }

    // ---- 3. Import each identity ------------------------------------
    int imported = 0;   // identities whose embedding got written
    int skipped  = 0;   // existing residents skipped (no --overwrite)
    int failed   = 0;   // rows that errored out

    for (const Identity& id : fdb.all()) {
        if (id.name.empty() || id.embedding.empty()) {
            std::fprintf(stderr, "[migrate] skip malformed identity "
                         "(name='%s', dim=%zu)\n",
                         id.name.c_str(), id.embedding.size());
            ++failed;
            continue;
        }

        const int64_t existing = db.find_resident(id.name);
        if (existing >= 0 && !overwrite) {
            std::printf("[migrate] skip existing resident: %s\n",
                        id.name.c_str());
            ++skipped;
            continue;
        }

        // upsert_resident returns the existing id if present, else creates a
        // new resident with home_floor = HOME_FLOOR_UNSET (0).
        int64_t rid = db.upsert_resident(id.name, HOME_FLOOR_UNSET);
        if (rid < 0) {
            std::fprintf(stderr, "[migrate] upsert_resident failed: %s\n",
                         id.name.c_str());
            ++failed;
            continue;
        }

        // Overwrite mode: clear old embeddings before re-adding so the
        // resident ends up with exactly the .fdb vector.
        if (existing >= 0 && overwrite) {
            if (!db.delete_embeddings(rid)) {
                std::fprintf(stderr, "[migrate] delete_embeddings failed for "
                             "%s\n", id.name.c_str());
                ++failed;
                continue;
            }
        }

        if (!db.add_embedding(rid, "id_photo", id.embedding)) {
            std::fprintf(stderr, "[migrate] add_embedding failed for %s\n",
                         id.name.c_str());
            ++failed;
            continue;
        }
        ++imported;
    }

    db.close();

    // ---- 4. Summary --------------------------------------------------
    std::printf("\n[migrate] done: imported=%d  skipped=%d  failed=%d\n",
                imported, skipped, failed);
    // Every migrated resident has home_floor=0 (no floor from .fdb). Warn so
    // HR can fill floors in later; the cabin greets by name but will not
    // auto-call a floor for these until updated (spec req §6).
    if (imported > 0) {
        std::printf("[migrate] WARNING: %d resident(s) imported with "
                    "home_floor=0 (\"no floor registered\"). Update floors in "
                    "the residents table before enabling auto floor call.\n",
                    imported);
    }

    return failed > 0 ? 1 : 0;
}
