#include "match_engine.h"
#include "resident_db.h"   // full definition of EmbeddingRow

#include <cstdio>

void MatchEngine::clear() {
    dim_ = 0;
    flat_.clear();
    owner_.clear();
}

void MatchEngine::build(const std::vector<EmbeddingRow>& rows, int dim) {
    clear();
    dim_ = dim;
    flat_.reserve(rows.size() * static_cast<size_t>(dim));
    owner_.reserve(rows.size());

    int skipped = 0;
    for (const auto& r : rows) {
        if (static_cast<int>(r.vector.size()) != dim) {
            ++skipped;
            continue;
        }
        flat_.insert(flat_.end(), r.vector.begin(), r.vector.end());
        owner_.push_back(r.resident_id);
    }
    if (skipped > 0) {
        std::fprintf(stderr,
            "[match] WARN: skipped %d embedding(s) with dim != %d\n",
            skipped, dim);
    }
}

void MatchEngine::build_raw(std::vector<float> flat,
                            std::vector<int64_t> owners,
                            int dim) {
    dim_   = dim;
    flat_  = std::move(flat);
    owner_ = std::move(owners);
}

MatchResult MatchEngine::match(const std::vector<float>& query,
                               float threshold) const {
    MatchResult res;
    if (dim_ <= 0 || owner_.empty()) return res;
    if (static_cast<int>(query.size()) != dim_) return res;

    const float* q = query.data();
    const size_t n = owner_.size();

    float  best_sim = -2.0f;   // cosine range is [-1, 1]
    size_t best_i   = 0;

    for (size_t i = 0; i < n; ++i) {
        const float* v = &flat_[i * static_cast<size_t>(dim_)];
        // Vectors are L2-normalized => cosine similarity == dot product.
        float dot = 0.0f;
        for (int d = 0; d < dim_; ++d) dot += q[d] * v[d];
        if (dot > best_sim) {
            best_sim = dot;
            best_i   = i;
        }
    }

    res.similarity = best_sim;
    res.emb_index  = static_cast<int>(best_i);
    if (best_sim >= threshold) {
        res.resident_id = owner_[best_i];
    }
    return res;
}
