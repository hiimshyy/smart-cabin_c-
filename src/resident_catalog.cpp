#include "resident_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

void hash_bytes(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= FNV_PRIME; }
}
template <typename T> void hash_value(uint64_t& h, const T& v) {
    hash_bytes(h, &v, sizeof(v));
}
void hash_string(uint64_t& h, const std::string& s) {
    const uint64_t n = static_cast<uint64_t>(s.size());
    hash_value(h, n);
    if (!s.empty()) hash_bytes(h, s.data(), s.size());
}
uint64_t metadata_hash(const Resident& r) {
    uint64_t h = FNV_OFFSET;
    hash_value(h, r.id); hash_string(h, r.name); hash_string(h, r.apartment);
    hash_value(h, r.home_floor); hash_string(h, r.language);
    hash_string(h, r.greeting_name); hash_string(h, r.role); hash_string(h, r.ext_id);
    return h;
}
uint64_t embedding_hash(const EmbeddingRow& e) {
    uint64_t h = FNV_OFFSET;
    // Deliberately exclude SQLite row id: delete+reinsert of identical runtime
    // content must not create a false catalog generation.
    hash_value(h, e.resident_id); hash_string(h, e.source);
    hash_value(h, e.declared_dim); hash_value(h, e.blob_nbytes);
    if (!e.vector.empty()) hash_bytes(h, e.vector.data(), e.vector.size() * sizeof(float));
    return h;
}
}

CatalogDiff diff_resident_catalogs(const std::map<int64_t, ResidentDigest>& old_digests,
                                   const std::map<int64_t, ResidentDigest>& new_digests) {
    CatalogDiff d;
    for (const auto& [id, digest] : new_digests) {
        auto it = old_digests.find(id);
        if (it == old_digests.end()) d.added.insert(id);
        else if (it->second != digest) d.changed.insert(id);
    }
    for (const auto& [id, _] : old_digests)
        if (new_digests.find(id) == new_digests.end()) d.removed.insert(id);
    return d;
}

bool build_resident_catalog(const std::vector<Resident>& residents,
                            const std::vector<EmbeddingRow>& embeddings,
                            int recog_dim,
                            ResidentCatalogCandidate& out,
                            std::string& error,
                            bool build_matcher) {
    error.clear();
    ResidentCatalogCandidate tmp;
    if (recog_dim <= 0) { error = "invalid_recog_dim"; return false; }

    std::vector<const Resident*> ordered_residents;
    ordered_residents.reserve(residents.size());
    for (const auto& r : residents) ordered_residents.push_back(&r);
    std::sort(ordered_residents.begin(), ordered_residents.end(),
              [](auto* a, auto* b){ return a->id < b->id; });
    for (const Resident* r : ordered_residents) {
        if (r->id < 0 || tmp.resident_by_id.count(r->id)) {
            error = "invalid_or_duplicate_resident_id"; return false;
        }
        tmp.resident_by_id[r->id] = *r;
        tmp.digests[r->id].metadata_hash = metadata_hash(*r);
        tmp.digests[r->id].embeddings_hash = FNV_OFFSET;
    }

    std::vector<EmbeddingRow> ordered_embeddings = embeddings;
    std::sort(ordered_embeddings.begin(), ordered_embeddings.end(),
              [](const EmbeddingRow& a, const EmbeddingRow& b) {
                  if (a.resident_id != b.resident_id) return a.resident_id < b.resident_id;
                  return a.id < b.id;
              });
    int64_t prev_id = -1;
    std::map<int64_t, std::vector<uint64_t>> embedding_hashes;
    for (const auto& e : ordered_embeddings) {
        if (e.id < 0 || e.id == prev_id) { error = "invalid_or_duplicate_embedding_id"; return false; }
        prev_id = e.id;
        auto owner = tmp.resident_by_id.find(e.resident_id);
        if (owner == tmp.resident_by_id.end()) { error = "embedding_owner_not_active"; return false; }
        if (!e.blob_valid) { error = "embedding_blob_null"; return false; }
        if (!e.metadata_valid) { error = "embedding_metadata_invalid"; return false; }
        if (e.declared_dim != recog_dim) { error = "embedding_dim_mismatch"; return false; }
        if (e.blob_nbytes != e.declared_dim * static_cast<int64_t>(sizeof(float))) {
            error = "embedding_blob_size_mismatch"; return false;
        }
        if (static_cast<int64_t>(e.vector.size()) != e.declared_dim) {
            error = "embedding_vector_size_mismatch"; return false;
        }
        double norm2 = 0.0;
        for (float v : e.vector) {
            if (!std::isfinite(v)) { error = "embedding_non_finite"; return false; }
            norm2 += static_cast<double>(v) * v;
        }
        const double norm = std::sqrt(norm2);
        if (norm <= 1e-8) { error = "embedding_zero_norm"; return false; }
        if (std::fabs(norm - 1.0) > 0.05) { error = "embedding_not_normalized"; return false; }
        embedding_hashes[e.resident_id].push_back(embedding_hash(e));
    }
    // Canonicalize each resident's embedding multiset by runtime content, not
    // storage row ID/order. Duplicate vectors remain visible through count.
    for (auto& [resident_id, hashes] : embedding_hashes) {
        std::sort(hashes.begin(), hashes.end());
        auto& h = tmp.digests[resident_id].embeddings_hash;
        for (uint64_t eh : hashes) hash_value(h, eh);
    }

    uint64_t all = FNV_OFFSET;
    for (const auto& [id, d] : tmp.digests) {
        hash_value(all, id); hash_value(all, d.metadata_hash); hash_value(all, d.embeddings_hash);
    }
    tmp.fingerprint.hash = all;
    tmp.fingerprint.resident_count = tmp.resident_by_id.size();
    tmp.fingerprint.embedding_count = ordered_embeddings.size();
    if (build_matcher) {
        tmp.matcher.build(ordered_embeddings, recog_dim);
        if (!ordered_embeddings.empty() && tmp.matcher.vector_count() != ordered_embeddings.size()) {
            error = "matcher_build_incomplete"; return false;
        }
    }
    out = std::move(tmp);
    return true;
}

bool ResidentCatalogReloader::open(const std::string& db_path, int recog_dim,
                                   int interval_ms, double now_ms, std::string& error) {
    close();
    db_path_ = db_path;
    recog_dim_ = recog_dim;
    interval_ms_ = std::max(0, interval_ms);
    next_poll_ms_ = now_ms;
    error.clear();
    if (!enabled()) return true;
    if (!reader_.open_readonly(db_path_, busy_timeout_ms_)) {
        error = "reader_open_failed";
        next_reopen_ms_ = now_ms + reopen_backoff_ms_;
        return false;
    }
    force_full_snapshot_ = true;
    return true;
}

bool ResidentCatalogReloader::ensure_reader(double now_ms, std::string& error) {
    if (reader_.is_open()) return true;
    if (now_ms < next_reopen_ms_) { error = "reader_reopen_backoff"; return false; }
    if (!reader_.open_readonly(db_path_, busy_timeout_ms_)) {
        error = "reader_reopen_failed";
        next_reopen_ms_ = now_ms + reopen_backoff_ms_;
        reopen_backoff_ms_ = std::min(reopen_backoff_ms_ * 2, 10000);
        return false;
    }
    reopen_backoff_ms_ = 1000;
    force_full_snapshot_ = true;
    have_accepted_version_ = false;
    have_observed_version_ = false;
    return true;
}

void ResidentCatalogReloader::reader_failed(double now_ms) {
    reader_.close();
    pending_retry_ = true;
    force_full_snapshot_ = true;
    have_accepted_version_ = false;
    have_observed_version_ = false;
    next_reopen_ms_ = now_ms + reopen_backoff_ms_;
}

ResidentCatalogReloader::PollResult ResidentCatalogReloader::poll(
    double now_ms, ResidentCatalogCandidate& candidate, std::string& error) {
    error.clear();
    if (!enabled() || now_ms < next_poll_ms_) return PollResult::NotDue;
    next_poll_ms_ = now_ms + interval_ms_;
    ++stats_.polls;
    if (!ensure_reader(now_ms, error)) { ++stats_.reload_failure; return PollResult::Failed; }

    int64_t version = -1;
    if (!reader_.data_version(version)) {
        error = "data_version_failed"; reader_failed(now_ms);
        ++stats_.reload_failure; return PollResult::Failed;
    }
    if (have_observed_version_ && version != last_observed_version_)
        ++stats_.version_changes;
    last_observed_version_ = version;
    have_observed_version_ = true;
    if (!force_full_snapshot_ && !pending_retry_ && have_accepted_version_ &&
        version == accepted_data_version_) return PollResult::NoDbChange;
    ++stats_.full_scans;

    std::vector<Resident> residents;
    std::vector<EmbeddingRow> embeddings;
    if (!reader_.load_active_snapshot(residents, embeddings)) {
        error = "snapshot_failed"; pending_retry_ = true;
        ++stats_.reload_failure; return PollResult::Failed;
    }
    ResidentCatalogCandidate tmp;
    // Analyze/validate/hash first. Audit-only commits stop at the fingerprint
    // comparison and never allocate/copy a new matcher.
    if (!build_resident_catalog(residents, embeddings, recog_dim_, tmp, error, false)) {
        pending_retry_ = true; ++stats_.reload_failure; return PollResult::Failed;
    }
    if (generation_ > 0 && tmp.fingerprint == active_fingerprint_) {
        accepted_data_version_ = version;
        have_accepted_version_ = true;
        force_full_snapshot_ = false;
        pending_retry_ = false;
        return PollResult::NoCatalogChange;
    }
    tmp.matcher.build(embeddings, recog_dim_);
    if (!embeddings.empty() && tmp.matcher.vector_count() != embeddings.size()) {
        error = "matcher_build_incomplete";
        pending_retry_ = true; ++stats_.reload_failure; return PollResult::Failed;
    }
    ++stats_.matcher_builds;

    tmp.generation = generation_ + 1;
    tmp.observed_data_version = version;
    tmp.diff = diff_resident_catalogs(active_digests_, tmp.digests);
    candidate = std::move(tmp);
    ++stats_.content_changes;
    return PollResult::Reloaded;
}

void ResidentCatalogReloader::accept(const ResidentCatalogCandidate& candidate) {
    generation_ = candidate.generation;
    active_fingerprint_ = candidate.fingerprint;
    active_digests_ = candidate.digests;
    accepted_data_version_ = candidate.observed_data_version;
    have_accepted_version_ = true;
    force_full_snapshot_ = false;
    pending_retry_ = false;
    ++stats_.reload_success;
}

void ResidentCatalogReloader::close() {
    reader_.close();
    db_path_.clear();
    recog_dim_ = 0; interval_ms_ = 0;
    next_poll_ms_ = next_reopen_ms_ = 0;
    reopen_backoff_ms_ = 1000;
    force_full_snapshot_ = true; pending_retry_ = false;
    have_accepted_version_ = false; accepted_data_version_ = -1;
    have_observed_version_ = false; last_observed_version_ = -1;
    generation_ = 0; active_fingerprint_ = {};
    active_digests_.clear(); stats_ = {};
}
