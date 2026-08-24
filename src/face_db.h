#pragma once
// Face identity database.
// Storage: linear list of {name, L2-normalized embedding}.
// Matching: cosine similarity == dot product on L2-normalized vectors.
// File format .fdb (little-endian):
//   magic       : "FDB1"  (4 bytes)
//   dim         : int32
//   count       : int32
//   for each entry:
//     name_len  : uint16
//     name      : name_len bytes UTF-8
//     embedding : dim * float32

#include <string>
#include <vector>
#include <cstdint>

struct Identity {
    std::string        name;
    std::vector<float> embedding;    // L2-normalized, size == dim
};

class FaceDB {
public:
    FaceDB() = default;

    void   set_dim(int dim) { dim_ = dim; }
    int    dim()   const   { return dim_; }
    size_t size()  const   { return identities_.size(); }
    const std::vector<Identity>& all() const { return identities_; }

    // Lookup identity by name. Returns index or -1 if not found.
    int    find(const std::string& name) const;

    // Remove identity by index. Returns false if invalid index.
    bool   remove_at(size_t index);

    // Enroll or update: caller passes a set of embeddings for the same
    // identity. They are averaged and re-normalized.
    void add(const std::string& name,
             const std::vector<std::vector<float>>& embeddings);

    void clear() { identities_.clear(); }

    // Find best match: returns index or -1 if not found or below threshold.
    // Fills sim_out with the winning cosine similarity.
    int  match(const std::vector<float>& query,
               float threshold,
               float& sim_out) const;

    // I/O
    bool save(const std::string& path) const;
    bool load(const std::string& path);

private:
    int                    dim_ = 0;
    std::vector<Identity>  identities_;
};

// Cosine similarity assuming both are already L2-normalized (dot product).
float cosine_sim_normalized(const std::vector<float>& a,
                            const std::vector<float>& b);
