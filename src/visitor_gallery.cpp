#include "visitor_gallery.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace {

float dot_vec(const float* a, const float* b, int dim) {
    float s = 0.0f;
    for (int i = 0; i < dim; ++i) {
        s += a[i] * b[i];
    }
    return s;
}

bool normalize_vec(std::vector<float>& v) {
    double sum_sq = 0.0;
    for (float x : v) {
        sum_sq += static_cast<double>(x) * static_cast<double>(x);
    }
    if (sum_sq <= 1e-12) return false;
    const float inv_norm = static_cast<float>(1.0 / std::sqrt(sum_sq));
    for (float& x : v) {
        x *= inv_norm;
    }
    return true;
}

} // namespace

VisitorGallery::VisitorGallery(VisitorGalleryConfig config) : config_(config) {}

void VisitorGallery::clear() {
    flat_.clear();
    owner_visitors_.clear();
    emb_ids_.clear();
    emb_qualities_.clear();
    visitor_slots_.clear();
    next_synthetic_visitor_id_ = 1;
    next_synthetic_emb_id_     = 1;
}

void VisitorGallery::append_slot(std::int64_t visitor_id, std::int64_t emb_id, float quality,
                                 const std::vector<float>& vec) {
    const std::size_t slot = owner_visitors_.size();
    flat_.insert(flat_.end(), vec.begin(), vec.end());
    owner_visitors_.push_back(visitor_id);
    emb_ids_.push_back(emb_id);
    emb_qualities_.push_back(quality);
    visitor_slots_[visitor_id].push_back(slot);
}

void VisitorGallery::load(const std::vector<VisitorRecord>& active_visitors) {
    clear();
    std::int64_t max_vid = 0;
    std::int64_t max_eid = 0;
    for (const auto& v : active_visitors) {
        if (v.id <= 0) continue;
        max_vid = std::max(max_vid, v.id);
        visitor_slots_[v.id]; // ensure visitor entry even if embeddings empty
        for (const auto& e : v.embeddings) {
            if (static_cast<int>(e.vector.size()) != config_.dim) continue;
            std::vector<float> norm_vec = e.vector;
            if (!normalize_vec(norm_vec)) continue;
            max_eid = std::max(max_eid, e.id);
            append_slot(v.id, e.id, e.quality, norm_vec);
        }
    }
    next_synthetic_visitor_id_ = max_vid + 1;
    next_synthetic_emb_id_     = max_eid + 1;
}

VisitorResolveResult VisitorGallery::resolve(const std::vector<FaceSample>& samples,
                                             std::int64_t seen_ms, TrafficDB* db) {
    VisitorResolveResult res;
    if (config_.dim <= 0) return res;

    struct NormSample {
        std::vector<float> vec;
        float              quality = 0.0f;
    };
    std::vector<NormSample> valid;
    valid.reserve(samples.size());
    for (const auto& s : samples) {
        if (static_cast<int>(s.embedding.size()) != config_.dim) continue;
        NormSample ns;
        ns.vec     = s.embedding;
        ns.quality = s.quality;
        if (!normalize_vec(ns.vec)) continue;
        valid.push_back(std::move(ns));
    }
    if (static_cast<int>(valid.size()) < config_.min_consistent_samples) {
        return res;
    }

    // 1. Compute initial centroid and reject outliers with cos < 0.5.
    std::vector<float> centroid(static_cast<std::size_t>(config_.dim), 0.0f);
    for (const auto& s : valid) {
        for (int i = 0; i < config_.dim; ++i) {
            centroid[static_cast<std::size_t>(i)] += s.vec[static_cast<std::size_t>(i)];
        }
    }
    if (!normalize_vec(centroid)) return res;

    std::vector<NormSample> kept;
    kept.reserve(valid.size());
    for (auto& s : valid) {
        if (dot_vec(s.vec.data(), centroid.data(), config_.dim) >=
            config_.outlier_centroid_min_cos) {
            kept.push_back(std::move(s));
        }
    }
    res.kept_samples = static_cast<int>(kept.size());
    if (static_cast<int>(kept.size()) < config_.min_consistent_samples) {
        return res;
    }

    // 2. Recompute normalized centroid of kept samples and pick best quality sample.
    std::fill(centroid.begin(), centroid.end(), 0.0f);
    std::size_t best_sample_idx = 0;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (kept[i].quality > kept[best_sample_idx].quality) {
            best_sample_idx = i;
        }
        for (int d = 0; d < config_.dim; ++d) {
            centroid[static_cast<std::size_t>(d)] += kept[i].vec[static_cast<std::size_t>(d)];
        }
    }
    if (!normalize_vec(centroid)) return res;
    const NormSample& best_sample = kept[best_sample_idx];

    // 3. Linear scan over flat_ to compute max cosine similarity per visitor.
    std::unordered_map<std::int64_t, float> max_sim_by_visitor;
    const std::size_t                       n_emb = owner_visitors_.size();
    for (std::size_t i = 0; i < n_emb; ++i) {
        const float*       vec = flat_.data() + i * static_cast<std::size_t>(config_.dim);
        const float        cos = dot_vec(centroid.data(), vec, config_.dim);
        const std::int64_t vid = owner_visitors_[i];
        const auto         it  = max_sim_by_visitor.find(vid);
        if (it == max_sim_by_visitor.end() || cos > it->second) {
            max_sim_by_visitor[vid] = cos;
        }
    }

    std::int64_t best_vid      = -1;
    float        best_sim      = -std::numeric_limits<float>::infinity();
    bool         has_runner_up = false;
    float        runner_up_sim = -std::numeric_limits<float>::infinity();
    for (const auto& kv : max_sim_by_visitor) {
        if (kv.second > best_sim) {
            if (best_vid >= 0) {
                has_runner_up = true;
                runner_up_sim = best_sim;
            }
            best_vid = kv.first;
            best_sim = kv.second;
        } else if (!has_runner_up || kv.second > runner_up_sim) {
            has_runner_up = true;
            runner_up_sim = kv.second;
        }
    }

    const bool matched_existing =
        best_vid >= 0 && best_sim >= config_.match_threshold &&
        (!has_runner_up || (best_sim - runner_up_sim >= config_.runner_up_margin));

    if (matched_existing) {
        res.resolved    = true;
        res.created_new = false;
        res.visitor_id  = best_vid;
        res.similarity  = best_sim;

        auto& slots = visitor_slots_[best_vid];
        if (static_cast<int>(slots.size()) < config_.max_embeddings_per_visitor) {
            std::int64_t new_emb_id = next_synthetic_emb_id_++;
            if (db && db->is_open()) {
                if (!db->record_visitor_observation(best_vid, seen_ms, &best_sample.vec,
                                                    best_sample.quality, -1, new_emb_id)) {
                    return res;
                }
            }
            append_slot(best_vid, new_emb_id, best_sample.quality, best_sample.vec);
        } else {
            // Find lowest-quality embedding slot for this visitor.
            std::size_t min_pos = 0;
            for (std::size_t k = 1; k < slots.size(); ++k) {
                if (emb_qualities_[slots[k]] < emb_qualities_[slots[min_pos]]) {
                    min_pos = k;
                }
            }
            const std::size_t slot_idx       = slots[min_pos];
            const bool        should_replace = best_sample.quality > emb_qualities_[slot_idx];
            std::int64_t      emb_id         = emb_ids_[slot_idx];
            if (db && db->is_open()) {
                if (!db->record_visitor_observation(best_vid, seen_ms,
                                                    should_replace ? &best_sample.vec : nullptr,
                                                    best_sample.quality, emb_id, emb_id)) {
                    return res;
                }
            }
            if (should_replace) {
                float* dst = flat_.data() + slot_idx * static_cast<std::size_t>(config_.dim);
                std::copy(best_sample.vec.begin(), best_sample.vec.end(), dst);
                emb_qualities_[slot_idx] = best_sample.quality;
            }
        }
        return res;
    }

    // Create new visitor
    std::int64_t new_vid = -1;
    std::int64_t new_eid = -1;
    if (db && db->is_open()) {
        if (!db->create_visitor(seen_ms, best_sample.vec, best_sample.quality, new_vid, new_eid)) {
            return res;
        }
        next_synthetic_visitor_id_ = std::max(next_synthetic_visitor_id_, new_vid + 1);
        next_synthetic_emb_id_     = std::max(next_synthetic_emb_id_, new_eid + 1);
    } else {
        new_vid = next_synthetic_visitor_id_++;
        new_eid = next_synthetic_emb_id_++;
    }
    append_slot(new_vid, new_eid, best_sample.quality, best_sample.vec);
    res.resolved    = true;
    res.created_new = true;
    res.visitor_id  = new_vid;
    res.similarity  = -1.0f;
    return res;
}
