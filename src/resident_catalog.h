#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "match_engine.h"
#include "resident_db.h"

struct CatalogFingerprint {
    uint64_t hash = 0;
    size_t resident_count = 0;
    size_t embedding_count = 0;
    bool operator==(const CatalogFingerprint& o) const {
        return hash == o.hash && resident_count == o.resident_count &&
               embedding_count == o.embedding_count;
    }
    bool operator!=(const CatalogFingerprint& o) const { return !(*this == o); }
};

struct ResidentDigest {
    uint64_t metadata_hash = 0;
    uint64_t embeddings_hash = 0;
    bool operator==(const ResidentDigest& o) const {
        return metadata_hash == o.metadata_hash && embeddings_hash == o.embeddings_hash;
    }
    bool operator!=(const ResidentDigest& o) const { return !(*this == o); }
};

struct CatalogDiff {
    std::set<int64_t> added;
    std::set<int64_t> changed;
    std::set<int64_t> removed;
    bool empty() const { return added.empty() && changed.empty() && removed.empty(); }
};

struct ResidentCatalogCandidate {
    uint64_t generation = 0;
    int64_t observed_data_version = -1;
    CatalogFingerprint fingerprint;
    std::map<int64_t, ResidentDigest> digests;
    std::map<int64_t, Resident> resident_by_id;
    MatchEngine matcher;
    CatalogDiff diff;
};

bool build_resident_catalog(const std::vector<Resident>& residents,
                            const std::vector<EmbeddingRow>& embeddings,
                            int recog_dim,
                            ResidentCatalogCandidate& out,
                            std::string& error,
                            bool build_matcher = true);

CatalogDiff diff_resident_catalogs(const std::map<int64_t, ResidentDigest>& old_digests,
                                   const std::map<int64_t, ResidentDigest>& new_digests);

class ResidentCatalogReloader {
public:
    enum class PollResult { NotDue, NoDbChange, NoCatalogChange, Reloaded, Failed };
    struct Stats {
        uint64_t polls = 0;
        uint64_t version_changes = 0;
        uint64_t full_scans = 0;
        uint64_t content_changes = 0;
        uint64_t matcher_builds = 0;
        uint64_t reload_success = 0;
        uint64_t reload_failure = 0;
    };

    bool open(const std::string& db_path, int recog_dim, int interval_ms,
              double now_ms, std::string& error);
    PollResult poll(double now_ms, ResidentCatalogCandidate& candidate,
                    std::string& error);
    void accept(const ResidentCatalogCandidate& candidate);
    void close();

    double next_deadline_ms() const { return next_poll_ms_; }
    uint64_t generation() const { return generation_; }
    const CatalogFingerprint& fingerprint() const { return active_fingerprint_; }
    const std::map<int64_t, ResidentDigest>& digests() const { return active_digests_; }
    const Stats& stats() const { return stats_; }
    bool enabled() const { return interval_ms_ > 0; }

#ifdef RESIDENT_DB_TEST_HOOK
    void disconnect_reader_for_test() { reader_.close(); }
    void set_reader_snapshot_hook_for_test(ResidentDB::SnapshotTestHook hook, void* ctx) {
        reader_.set_snapshot_test_hook(hook, ctx);
    }
#endif

private:
    bool ensure_reader(double now_ms, std::string& error);
    void reader_failed(double now_ms);

    ResidentDB reader_;
    std::string db_path_;
    int recog_dim_ = 0;
    int interval_ms_ = 0;
    int busy_timeout_ms_ = 50;
    double next_poll_ms_ = 0;
    double next_reopen_ms_ = 0;
    int reopen_backoff_ms_ = 1000;
    bool force_full_snapshot_ = true;
    bool pending_retry_ = false;
    bool have_accepted_version_ = false;
    int64_t accepted_data_version_ = -1;
    bool have_observed_version_ = false;
    int64_t last_observed_version_ = -1;
    uint64_t generation_ = 0;
    CatalogFingerprint active_fingerprint_;
    std::map<int64_t, ResidentDigest> active_digests_;
    Stats stats_;
};
