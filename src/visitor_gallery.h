#pragma once

#include "traffic_db.h"
#include "trip_assembler.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct VisitorGalleryConfig {
    int   dim                        = 512;
    float match_threshold            = 0.45f;
    float runner_up_margin           = 0.05f;
    float outlier_centroid_min_cos   = 0.50f;
    int   min_consistent_samples     = 2;
    int   max_embeddings_per_visitor = 10;
};

struct VisitorResolveResult {
    bool         resolved     = false; // true = visitor_id valid; false = unidentified
    bool         created_new  = false;
    std::int64_t visitor_id   = -1;
    float        similarity   = -1.0f;
    int          kept_samples = 0;
};

class VisitorGallery {
public:
    explicit VisitorGallery(VisitorGalleryConfig config = {});

    void load(const std::vector<VisitorRecord>& active_visitors);
    void clear();

    VisitorResolveResult resolve(const std::vector<FaceSample>& samples, std::int64_t seen_ms,
                                 TrafficDB* db = nullptr);

    std::size_t visitor_count() const {
        return visitor_slots_.size();
    }
    std::size_t embedding_count() const {
        return owner_visitors_.size();
    }

private:
    void append_slot(std::int64_t visitor_id, std::int64_t emb_id, float quality,
                     const std::vector<float>& vec);

    VisitorGalleryConfig                             config_;
    std::vector<float>                               flat_;
    std::vector<std::int64_t>                        owner_visitors_;
    std::vector<std::int64_t>                        emb_ids_;
    std::vector<float>                               emb_qualities_;
    std::map<std::int64_t, std::vector<std::size_t>> visitor_slots_;
    std::int64_t                                     next_synthetic_visitor_id_ = 1;
    std::int64_t                                     next_synthetic_emb_id_     = 1;
};
