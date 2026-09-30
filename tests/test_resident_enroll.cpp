#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "resident_db.h"
#include "resident_enroll.h"

static int checks = 0, failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static std::vector<float> vec(int dim, float seed) {
    std::vector<float> v(dim);
    double norm = 0;
    for (int i = 0; i < dim; ++i) { v[i] = seed + 0.01f * i; norm += v[i] * v[i]; }
    norm = std::sqrt(norm);
    for (float& x : v) x = static_cast<float>(x / norm);
    return v;
}

static const Resident* find_ext(const std::vector<Resident>& rs, const std::string& ext) {
    for (const auto& r : rs) if (r.ext_id == ext) return &r;
    return nullptr;
}

int main(int argc, char** argv) {
    const std::string schema = argc > 1 ? argv[1] : "db/schema.sql";
    const std::string path = "/tmp/test_resident_enroll.db";
    std::remove(path.c_str()); std::remove((path + "-wal").c_str()); std::remove((path + "-shm").c_str());

    ResidentDB db;
    CHECK(db.open(path, schema, false), "open temp DB");

    ResidentEnrollRequest seed;
    seed.ext_id = "EXT-1"; seed.name = "Old"; seed.home_floor = 2;
    seed.role = "staff"; seed.replace = true; seed.embeddings = {vec(8, 1.0f)};
    auto s = write_resident_enrollment(db, seed);
    CHECK(s.ok && s.embeddings_written == 1, "seed enrollment commits");

    // Deterministic mid-batch failure: first vector inserts, second empty vector
    // makes ResidentDB::add_embedding fail. The whole replace must roll back.
    ResidentEnrollRequest bad = seed;
    bad.name = "Changed"; bad.home_floor = 9; bad.role = "manager";
    bad.embeddings = {vec(8, 2.0f), {}};
    auto b = write_resident_enrollment(db, bad);
    CHECK(!b.ok && b.error == "db_write_failed", "mid-batch failure reported");

    std::vector<Resident> rs; std::vector<EmbeddingRow> es;
    CHECK(db.load_active(rs, es), "load after rollback");
    const Resident* r = find_ext(rs, "EXT-1");
    CHECK(r && r->name == "Old" && r->home_floor == 2 && r->role == "staff",
          "metadata rolled back");
    CHECK(es.size() == 1, "old embedding preserved after failed replace");

    // A failed new resident must not leave an empty/orphan resident row.
    ResidentEnrollRequest bad_new = bad;
    bad_new.ext_id = "EXT-NEW";
    auto bn = write_resident_enrollment(db, bad_new);
    CHECK(!bn.ok, "new resident batch fails");
    db.load_active(rs, es);
    CHECK(find_ext(rs, "EXT-NEW") == nullptr, "failed new resident rolled back");

    ResidentEnrollRequest good = seed;
    good.name = "Updated"; good.home_floor = 6; good.role = "engineer";
    good.embeddings = {vec(8, 3.0f), vec(8, 4.0f)};
    auto g = write_resident_enrollment(db, good);
    CHECK(g.ok && g.resident_id == s.resident_id && g.embeddings_written == 2,
          "successful replace commits full batch on same resident");
    db.load_active(rs, es);
    r = find_ext(rs, "EXT-1");
    CHECK(r && r->name == "Updated" && r->home_floor == 6 && r->role == "engineer",
          "successful metadata committed");
    CHECK(es.size() == 2, "successful batch has all embeddings");

    db.close();
    std::remove(path.c_str()); std::remove((path + "-wal").c_str()); std::remove((path + "-shm").c_str());
    std::printf("\n[test_resident_enroll] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
