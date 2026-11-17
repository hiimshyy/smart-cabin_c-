#include <cmath>
#include <cstdio>
#include <vector>

#include "visitor_gallery.h"

namespace {

int checks = 0;
int failed = 0;

#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

FaceSample make_vec_sample(float x0, float x1, float x2, float x3, float quality) {
    FaceSample s;
    s.embedding = {x0, x1, x2, x3};
    float norm  = 0.0f;
    for (float v : s.embedding)
        norm += v * v;
    norm = std::sqrt(norm);
    for (float& v : s.embedding)
        v /= norm;
    s.quality = quality;
    return s;
}

} // namespace

int main() {
    VisitorGalleryConfig cfg;
    cfg.dim                        = 4;
    cfg.match_threshold            = 0.70f;
    cfg.runner_up_margin           = 0.05f;
    cfg.outlier_centroid_min_cos   = 0.50f;
    cfg.min_consistent_samples     = 2;
    cfg.max_embeddings_per_visitor = 3;

    VisitorGallery gallery(cfg);

    // Fewer than 2 consistent samples after outlier rejection -> unidentified.
    {
        auto res = gallery.resolve({make_vec_sample(1.0f, 0.0f, 0.0f, 0.0f, 0.9f),
                                    make_vec_sample(-1.0f, 0.0f, 0.0f, 0.0f, 0.8f)},
                                   1000, nullptr);
        CHECK(!res.resolved && res.visitor_id < 0,
              "opposite vectors rejected by centroid outlier filter leaving <2 samples");
    }

    // 2 matching + 1 orthogonal outlier -> keeps 2, creates visitor 1.
    std::int64_t v1 = -1;
    {
        auto res = gallery.resolve({make_vec_sample(1.0f, 0.0f, 0.0f, 0.0f, 0.8f),
                                    make_vec_sample(0.99f, 0.05f, 0.0f, 0.0f, 0.9f),
                                    make_vec_sample(0.0f, 0.0f, 1.0f, 0.0f, 0.7f)},
                                   2000, nullptr);
        CHECK(res.resolved && res.created_new && res.kept_samples == 2 &&
                  gallery.visitor_count() == 1,
              "outlier removed while keeping 2 consistent samples to create new visitor 1");
        v1 = res.visitor_id;
    }

    // Matching existing visitor adds up to M=3 embeddings, then replaces lowest-quality slot.
    {
        auto r2 = gallery.resolve({make_vec_sample(0.98f, 0.04f, 0.0f, 0.0f, 0.4f),
                                   make_vec_sample(0.99f, 0.02f, 0.0f, 0.0f, 0.5f)},
                                  3000, nullptr);
        CHECK(r2.resolved && !r2.created_new && r2.visitor_id == v1 &&
                  gallery.embedding_count() == 2,
              "second trip matches existing visitor 1 and adds 2nd embedding");

        auto r3 = gallery.resolve({make_vec_sample(1.0f, 0.01f, 0.0f, 0.0f, 0.6f),
                                   make_vec_sample(0.99f, 0.03f, 0.0f, 0.0f, 0.65f)},
                                  4000, nullptr);
        CHECK(r3.resolved && r3.visitor_id == v1 && gallery.embedding_count() == 3,
              "third trip fills M=3 capacity");

        auto r4 = gallery.resolve({make_vec_sample(1.0f, 0.0f, 0.01f, 0.0f, 0.95f),
                                   make_vec_sample(0.99f, 0.0f, 0.02f, 0.0f, 0.92f)},
                                  5000, nullptr);
        CHECK(r4.resolved && r4.visitor_id == v1 && gallery.embedding_count() == 3,
              "fourth trip keeps embedding count capped at M=3 by replacing lowest quality");
    }

    // Create distinct second visitor v2 and test runner-up margin rejection.
    std::int64_t v2 = -1;
    {
        auto r5 = gallery.resolve({make_vec_sample(0.0f, 1.0f, 0.0f, 0.0f, 0.85f),
                                   make_vec_sample(0.02f, 0.99f, 0.0f, 0.0f, 0.88f)},
                                  6000, nullptr);
        CHECK(r5.resolved && r5.created_new && r5.visitor_id != v1 && gallery.visitor_count() == 2,
              "orthogonal visitor creates new visitor 2");
        v2 = r5.visitor_id;
    }

    // Midpoint vector equidistant between v1=[1,0,0,0] and v2=[0,1,0,0] has ~0.707 to both
    // (above threshold 0.70, margin ~0.00 < 0.05) -> must NOT merge, creates new visitor instead.
    {
        auto r6 = gallery.resolve({make_vec_sample(0.707f, 0.707f, 0.0f, 0.0f, 0.8f),
                                   make_vec_sample(0.710f, 0.704f, 0.0f, 0.0f, 0.82f)},
                                  7000, nullptr);
        CHECK(r6.resolved && r6.created_new && r6.visitor_id != v1 && r6.visitor_id != v2 &&
                  gallery.visitor_count() == 3,
              "ambiguous best vs runner-up below 0.05 margin does not assign either visitor");
    }

    std::printf("\n[test_visitor_gallery] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
