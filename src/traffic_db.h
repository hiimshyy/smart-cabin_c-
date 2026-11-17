#pragma once

#include "call_tracker.h"
#include "elevator_state.h"
#include "trip_assembler.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct sqlite3;

struct TrafficWriteBatch {
    std::vector<CabinStopRecord>     stops;
    std::vector<CabinRunRecord>      runs;
    std::vector<RunPassengerLink>    run_passengers;
    std::vector<PassengerTripRecord> trips;
    std::vector<CarCallRecord>       car_calls;
    std::vector<HallCallRecord>      hall_calls;
};

struct TrafficPurgeResult {
    std::int64_t rows_deleted     = 0;
    std::int64_t visitors_expired = 0;
};

struct VisitorEmbeddingData {
    std::int64_t       id         = -1;
    std::int64_t       visitor_id = -1;
    std::vector<float> vector;
    float              quality       = 0.0f;
    std::int64_t       created_at_ms = 0;
};

struct VisitorRecord {
    std::int64_t                      id            = -1;
    std::int64_t                      first_seen_ms = 0;
    std::int64_t                      last_seen_ms  = 0;
    int                               trip_count    = 0;
    std::string                       status;
    std::int64_t                      merged_into          = -1;
    std::int64_t                      promoted_resident_id = -1;
    std::vector<VisitorEmbeddingData> embeddings;
};

struct VisitorSummary {
    std::int64_t id              = -1;
    std::int64_t first_seen_ms   = 0;
    std::int64_t last_seen_ms    = 0;
    int          trip_count      = 0;
    int          embedding_count = 0;
    float        max_quality     = 0.0f;
    std::string  status;
    std::int64_t merged_into          = -1;
    std::int64_t promoted_resident_id = -1;
};

struct VisitorShowResult {
    VisitorSummary                    summary;
    std::vector<VisitorEmbeddingData> embeddings;
    std::vector<PassengerTripRecord>  recent_trips;
};

struct PromoteRequest {
    std::int64_t visitor_id           = -1;
    std::int64_t existing_resident_id = -1;
    std::string  new_name;
    int          home_floor = -1;
    std::string  role       = "staff";
    std::string  resident_db_path;
    std::string  resident_schema_path = "db/schema.sql";
    std::int64_t now_ms               = 0;
};

struct PromoteResult {
    bool         ok                = false;
    std::int64_t resident_id       = -1;
    int          embeddings_copied = 0;
    std::int64_t trips_updated     = 0;
    std::string  error;
};

struct PromotionSuggestion {
    std::int64_t visitor_id  = -1;
    std::int64_t resident_id = -1;
    std::string  resident_name;
    int          home_floor     = -1;
    float        max_similarity = 0.0f;
    int          trip_count     = 0;
};

struct TrafficExportResult {
    bool                       ok = false;
    std::map<std::string, int> row_counts;
    std::string                error;
};

class TrafficDB {
public:
    TrafficDB() = default;
    ~TrafficDB();

    TrafficDB(const TrafficDB&)            = delete;
    TrafficDB& operator=(const TrafficDB&) = delete;

    bool open(const std::string& db_path,
              const std::string& schema_sql_path = "db/traffic_schema.sql");
    void close();
    bool is_open() const {
        return db_ != nullptr;
    }

    bool insert_session(const std::string& session_key, std::int64_t started_at_ms, int cabin_id,
                        int elevator_slave, std::string* error_out = nullptr);

    bool insert_batch(const TrafficWriteBatch& batch, std::string* error_out = nullptr);

    bool purge_expired(std::int64_t now_ms, int traffic_retention_days, int visitor_retention_days,
                       TrafficPurgeResult& result, std::string* error_out = nullptr);

    // Visitor gallery persistence API
    bool load_active_visitors(std::vector<VisitorRecord>& out, int expected_dim = 512) const;
    bool create_visitor(std::int64_t seen_ms, const std::vector<float>& embedding, float quality,
                        std::int64_t& new_visitor_id, std::int64_t& new_embedding_id);
    bool record_visitor_observation(std::int64_t visitor_id, std::int64_t seen_ms,
                                    const std::vector<float>* new_embedding, float quality,
                                    std::int64_t  replace_embedding_id,
                                    std::int64_t& out_embedding_id);

    // Admin & export operations
    bool          list_visitors(const std::string& status_filter, int limit,
                                std::vector<VisitorSummary>& out) const;
    bool          show_visitor(std::int64_t visitor_id, VisitorShowResult& out) const;
    bool          merge_visitors(std::int64_t from_id, std::int64_t into_id, std::int64_t now_ms,
                                 int max_embeddings_per_visitor, std::string& error);
    bool          delete_visitor(std::int64_t visitor_id, std::int64_t now_ms,
                                 std::vector<std::string>& face_images_to_unlink, std::string& error);
    PromoteResult promote_visitor(const PromoteRequest& req);
    bool suggest_promotions(const std::string& resident_db_path, int recog_dim, int min_trips,
                            float min_sim, std::vector<PromotionSuggestion>& out,
                            std::string& error) const;
    TrafficExportResult export_csv(const std::string& out_dir, std::int64_t from_ms,
                                   std::int64_t to_ms) const;

private:
    bool has_tables() const;
    bool apply_schema(const std::string& schema_sql_path);
    bool delete_chunk_by_key(const char* table, const char* key_col, const char* where_clause,
                             std::int64_t cutoff_ms, std::int64_t& total_deleted,
                             std::string* error_out);
    bool insert_audit(std::int64_t now_ms, const std::string& action, std::int64_t visitor_id,
                      std::int64_t resident_id, const std::string& detail);

    sqlite3* db_ = nullptr;
};
