#include "match_engine.h"
#include "resident_db.h"   // full definition of EmbeddingRow
#include "log/logger.h"

#include <cstdio>
#include <algorithm>
#include <map>

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
        LOG_WARN("match", "skipped %d embedding(s) with dim != %d",
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
    return match_top2(query, threshold).best;
}

MatchTop2 MatchEngine::match_top2(const std::vector<float>& query,
                                  float threshold) const {
    MatchTop2 out;
    if (dim_ <= 0 || owner_.empty()) return out;
    if (static_cast<int>(query.size()) != dim_) return out;

    const float* q = query.data();
    const size_t n = owner_.size();

    std::map<int64_t, std::pair<float, size_t>> best_per_resident;

    for (size_t i = 0; i < n; ++i) {
        const float* v = &flat_[i * static_cast<size_t>(dim_)];
        // Vectors are L2-normalized => cosine similarity == dot product.
        float dot = 0.0f;
        for (int d = 0; d < dim_; ++d) dot += q[d] * v[d];
        auto it = best_per_resident.find(owner_[i]);
        if (it == best_per_resident.end() || dot > it->second.first)
            best_per_resident[owner_[i]] = {dot, i};
    }
    std::vector<std::pair<float, size_t>> ranked;
    ranked.reserve(best_per_resident.size());
    for (const auto& item : best_per_resident) ranked.push_back(item.second);
    std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return owner_[a.second] < owner_[b.second];
    });
    auto make_result = [&](size_t rank) {
        MatchResult result;
        if (rank >= ranked.size()) return result;
        result.similarity = ranked[rank].first;
        result.emb_index = static_cast<int>(ranked[rank].second);
        if (result.similarity >= threshold) result.resident_id = owner_[ranked[rank].second];
        return result;
    };
    out.best = make_result(0);
    out.runner_up = make_result(1);
    // The runner-up's raw similarity is needed for the conflict margin even
    // when it falls below the ordinary match threshold.
    if (ranked.size() > 1) {
        out.runner_up.resident_id = owner_[ranked[1].second];
    }
    return out;
}
