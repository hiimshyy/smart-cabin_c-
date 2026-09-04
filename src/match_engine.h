#pragma once
// Multi-embedding matching engine (spec resident-db-layer, R2).
//
// Unlike FaceDB (which averages all embeddings of a person into a single
// vector), MatchEngine keeps EVERY embedding of every resident and matches
// against all of them, taking the maximum cosine similarity. This preserves
// pose/lighting variation and works much better with low-quality ID photos.
//
// Storage: one contiguous flat float array (count * dim) for cache-friendly
// linear scan, plus a parallel owner[] array mapping each vector to its
// resident_id. Vectors are assumed L2-normalized, so cosine == dot product.

#include <cstdint>
#include <vector>

// Forward-declared here to avoid a hard dependency on resident_db.h; the
// real definition lives there. MatchEngine only needs the fields below.
struct EmbeddingRow;

struct MatchResult {
    int64_t resident_id = -1;   // -1 => unknown (below threshold or empty DB)
    float   similarity  = -1.0f;
    int     emb_index   = -1;   // which stored vector won (debug / future use)
};

class MatchEngine {
public:
    MatchEngine() = default;

    // Build the flat index from a list of embedding rows. Rows whose vector
    // size != dim are skipped (with the count reflected in vector_count()).
    void build(const std::vector<EmbeddingRow>& rows, int dim);

    // Build directly from raw parallel arrays (used by unit tests without
    // pulling in the full EmbeddingRow / SQLite layer).
    //   flat  : owners.size() * dim floats, L2-normalized, row-major
    //   owners: resident_id for each stored vector
    void build_raw(std::vector<float> flat,
                   std::vector<int64_t> owners,
                   int dim);

    // Return best match over ALL stored vectors. If the winning cosine is
    // below `threshold`, resident_id stays -1 (unknown) but similarity still
    // reports the best score seen (useful for logging).
    MatchResult match(const std::vector<float>& query, float threshold) const;

    int    dim()          const { return dim_; }
    size_t vector_count() const { return owner_.size(); }
    bool   empty()        const { return owner_.empty(); }
    void   clear();

private:
    int                  dim_ = 0;
    std::vector<float>   flat_;    // vector_count * dim, contiguous
    std::vector<int64_t> owner_;   // vector_count, resident_id per vector
};
