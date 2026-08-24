#include "face_db.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>

float cosine_sim_normalized(const std::vector<float>& a,
                            const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += (double)a[i] * b[i];
    return static_cast<float>(s);
}

int FaceDB::find(const std::string& name) const {
    for (size_t i = 0; i < identities_.size(); ++i) {
        if (identities_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool FaceDB::remove_at(size_t index) {
    if (index >= identities_.size()) return false;
    identities_.erase(identities_.begin() + index);
    return true;
}

void FaceDB::add(const std::string& name,
                 const std::vector<std::vector<float>>& embeddings) {
    if (embeddings.empty()) return;
    if (dim_ == 0) dim_ = static_cast<int>(embeddings[0].size());

    std::vector<double> avg(dim_, 0.0);
    int n_valid = 0;
    for (const auto& e : embeddings) {
        if ((int)e.size() != dim_) continue;
        for (int i = 0; i < dim_; ++i) avg[i] += e[i];
        ++n_valid;
    }
    if (n_valid == 0) return;
    for (int i = 0; i < dim_; ++i) avg[i] /= n_valid;

    // Re-normalize
    double sq = 0.0;
    for (int i = 0; i < dim_; ++i) sq += avg[i] * avg[i];
    double inv = (sq > 1e-12) ? 1.0 / std::sqrt(sq) : 0.0;

    Identity id;
    id.name = name;
    id.embedding.resize(dim_);
    for (int i = 0; i < dim_; ++i) id.embedding[i] = static_cast<float>(avg[i] * inv);
    identities_.push_back(std::move(id));
}

int FaceDB::match(const std::vector<float>& q,
                  float threshold,
                  float& sim_out) const {
    sim_out = -1.0f;
    int best = -1;
    for (size_t i = 0; i < identities_.size(); ++i) {
        float s = cosine_sim_normalized(q, identities_[i].embedding);
        if (s > sim_out) {
            sim_out = s;
            best = static_cast<int>(i);
        }
    }
    if (sim_out < threshold) return -1;
    return best;
}

bool FaceDB::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const char magic[4] = {'F','D','B','1'};
    f.write(magic, 4);
    int32_t dim = dim_;
    int32_t n   = static_cast<int32_t>(identities_.size());
    f.write(reinterpret_cast<const char*>(&dim), 4);
    f.write(reinterpret_cast<const char*>(&n),   4);
    for (const auto& id : identities_) {
        uint16_t nl = static_cast<uint16_t>(id.name.size());
        f.write(reinterpret_cast<const char*>(&nl), 2);
        f.write(id.name.data(), nl);
        f.write(reinterpret_cast<const char*>(id.embedding.data()),
                dim_ * sizeof(float));
    }
    return f.good();
}

bool FaceDB::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4];
    f.read(magic, 4);
    if (std::memcmp(magic, "FDB1", 4) != 0) {
        fprintf(stderr, "[face_db] bad magic in %s\n", path.c_str());
        return false;
    }
    int32_t dim = 0, n = 0;
    f.read(reinterpret_cast<char*>(&dim), 4);
    f.read(reinterpret_cast<char*>(&n),   4);
    dim_ = dim;
    identities_.clear();
    identities_.reserve(n);
    for (int i = 0; i < n; ++i) {
        Identity id;
        uint16_t nl = 0;
        f.read(reinterpret_cast<char*>(&nl), 2);
        id.name.resize(nl);
        f.read(&id.name[0], nl);
        id.embedding.resize(dim_);
        f.read(reinterpret_cast<char*>(id.embedding.data()),
               dim_ * sizeof(float));
        identities_.push_back(std::move(id));
    }
    return f.good() || f.eof();
}
