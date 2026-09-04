// Unit test for MatchEngine (spec resident-db-layer, Task 2.2).
// No NPU / no SQLite required — pure vector math. Build with:
//   g++ -std=c++17 -I../src test_match_engine.cpp ../src/match_engine.cpp -o test_match_engine
//
// Uses build_raw() so it does not pull in the SQLite layer.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "match_engine.h"

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

// L2-normalize a raw vector in place.
static std::vector<float> norm(std::vector<float> v) {
    double s = 0;
    for (float x : v) s += (double)x * x;
    double inv = s > 1e-12 ? 1.0 / std::sqrt(s) : 0.0;
    for (float& x : v) x = (float)(x * inv);
    return v;
}

int main() {
    const int dim = 4;

    // Three residents, each with its own distinct direction.
    // resident 10 -> along axis 0, resident 20 -> axis 1, resident 30 -> axis 2
    std::vector<std::vector<float>> vecs = {
        norm({1, 0, 0, 0}),   // r10 a
        norm({0.9f, 0.1f, 0, 0}), // r10 b (still mostly axis0)
        norm({0, 1, 0, 0}),   // r20
        norm({0, 0, 1, 0}),   // r30
    };
    std::vector<int64_t> owners = {10, 10, 20, 30};

    // Flatten.
    std::vector<float> flat;
    for (auto& v : vecs) flat.insert(flat.end(), v.begin(), v.end());

    MatchEngine eng;
    eng.build_raw(flat, owners, dim);

    CHECK(eng.vector_count() == 4, "vector_count == 4");
    CHECK(eng.dim() == dim, "dim == 4");
    CHECK(!eng.empty(), "engine not empty");

    // Query near axis-0 should match resident 10 with high similarity.
    {
        auto q = norm({0.95f, 0.05f, 0, 0});
        MatchResult r = eng.match(q, 0.35f);
        CHECK(r.resident_id == 10, "axis0 query -> resident 10");
        CHECK(r.similarity > 0.9f, "axis0 query high similarity");
    }

    // Query along axis-1 should match resident 20.
    {
        auto q = norm({0, 1, 0, 0});
        MatchResult r = eng.match(q, 0.35f);
        CHECK(r.resident_id == 20, "axis1 query -> resident 20");
        CHECK(std::fabs(r.similarity - 1.0f) < 1e-4, "axis1 exact match sim ~1");
    }

    // Orthogonal query (axis-3) matches nothing above threshold -> unknown.
    {
        auto q = norm({0, 0, 0, 1});
        MatchResult r = eng.match(q, 0.35f);
        CHECK(r.resident_id == -1, "orthogonal query -> unknown");
        CHECK(r.similarity < 0.35f, "orthogonal query below threshold");
    }

    // Best-below-threshold still reports similarity but resident_id == -1.
    {
        auto q = norm({0.4f, 0.0f, 0.0f, 0.92f}); // small axis0 component
        MatchResult r = eng.match(q, 0.6f);
        CHECK(r.resident_id == -1, "weak match below 0.6 -> unknown");
        CHECK(r.similarity > 0.0f, "weak match still reports positive sim");
    }

    // Empty engine returns unknown safely.
    {
        MatchEngine e2;
        MatchResult r = e2.match(norm({1, 0, 0, 0}), 0.1f);
        CHECK(r.resident_id == -1, "empty engine -> unknown");
        CHECK(e2.empty(), "empty engine empty()");
    }

    // Dim mismatch query returns unknown without crashing.
    {
        std::vector<float> bad = {1, 0, 0};  // dim 3 != 4
        MatchResult r = eng.match(bad, 0.1f);
        CHECK(r.resident_id == -1, "dim-mismatch query -> unknown");
    }

    std::printf("\n[test_match_engine] %d checks, %d failed\n",
                g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
