#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "match_engine.h"
#include "resident_db.h"
#include "resident_enroll.h"
#include "traffic_db.h"

namespace {

int checks = 0;
int failed = 0;

#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

std::vector<float> make_unit_vec(int dim, int hot_idx, float second_val = 0.0f) {
    std::vector<float> v(static_cast<std::size_t>(dim), 0.0f);
    v[static_cast<std::size_t>(hot_idx % dim)] = 1.0f;
    if (second_val != 0.0f) {
        v[static_cast<std::size_t>((hot_idx + 1) % dim)] = second_val;
    }
    float norm = 0.0f;
    for (float x : v)
        norm += x * x;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& x : v)
            x /= norm;
    }
    return v;
}

std::string slurp_file(const std::string& path) {
    std::ifstream      in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    const std::string traffic_schema =
        (argc >= 2) ? argv[1] : "/home/orangepi/face_recog_app/db/traffic_schema.sql";
    const std::string resident_schema =
        (argc >= 3) ? argv[2] : "/home/orangepi/face_recog_app/db/schema.sql";

    const std::string base_dir = "/tmp/scabin_tests/traffic_db_test_" + std::to_string(::getpid());
    const std::string tdb_path = base_dir + "/traffic.db";
    const std::string rdb_path = base_dir + "/residents.db";
    const std::string csv_dir  = base_dir + "/csv";

    TrafficDB tdb;
    CHECK(tdb.open(tdb_path, traffic_schema), "open and initialize traffic.db");

    const std::int64_t now_ms = 1700000000000LL;
    CHECK(tdb.insert_session("sess1", now_ms, 2, 2), "insert app_sessions row");

    // Insert a batch with stops, runs, run_passengers, trips, car_calls, hall_calls.
    TrafficWriteBatch batch;
    CabinStopRecord   s1;
    s1.stop_key          = "s1";
    s1.cabin_id          = 2;
    s1.floor             = 0; // B
    s1.has_floor         = true;
    s1.arrived_at_ms     = static_cast<double>(now_ms);
    s1.door_opened_at_ms = static_cast<double>(now_ms + 1000);
    s1.departed_at_ms    = static_cast<double>(now_ms + 5000);
    s1.boarded           = 1;
    batch.stops.push_back(s1);

    CabinStopRecord s2;
    s2.stop_key      = "s2";
    s2.cabin_id      = 2;
    s2.floor         = 5;
    s2.has_floor     = true;
    s2.arrived_at_ms = static_cast<double>(now_ms + 20000);
    s2.alighted      = 1;
    batch.stops.push_back(s2);

    CabinRunRecord r1;
    r1.run_key         = "r1";
    r1.cabin_id        = 2;
    r1.from_stop_key   = "s1";
    r1.to_stop_key     = "s2";
    r1.from_floor      = 0;
    r1.has_from_floor  = true;
    r1.to_floor        = 5;
    r1.has_to_floor    = true;
    r1.direction       = "up";
    r1.departed_at_ms  = static_cast<double>(now_ms + 5000);
    r1.arrived_at_ms   = static_cast<double>(now_ms + 20000);
    r1.load_count      = 1;
    r1.has_load_count  = true;
    r1.load_confidence = "certain";
    r1.car_calls_mask  = (1u << 5);
    batch.runs.push_back(r1);

    std::int64_t vis1 = -1, emb1 = -1, vis2 = -1, emb2 = -1;
    CHECK(tdb.create_visitor(now_ms, make_unit_vec(8, 0), 0.7f, vis1, emb1) && vis1 > 0,
          "create visitor 1");
    CHECK(tdb.create_visitor(now_ms + 1000, make_unit_vec(8, 0, 0.1f), 0.9f, vis2, emb2) &&
              vis2 > 0,
          "create visitor 2");
    std::int64_t emb3    = -1;
    const auto   obs_vec = make_unit_vec(8, 0, 0.05f);
    CHECK(tdb.record_visitor_observation(vis1, now_ms + 500, &obs_vec, 0.8f, -1, emb3) && emb3 > 0,
          "record additional visitor observation");

    PassengerTripRecord tr;
    tr.trip_key          = "t1";
    tr.cabin_id          = 2;
    tr.entered_at_ms     = static_cast<double>(now_ms + 2000);
    tr.exited_at_ms      = static_cast<double>(now_ms + 22000);
    tr.floor_in          = 0;
    tr.has_floor_in      = true;
    tr.floor_out         = 5;
    tr.has_floor_out     = true;
    tr.identity_kind     = "visitor";
    tr.visitor_id        = vis1;
    tr.identity_sim      = 0.88f;
    tr.car_calls_mask    = (1u << 5);
    tr.rides             = 1;
    tr.boarded_stop_key  = "s1";
    tr.alighted_stop_key = "s2";
    tr.confidence        = "high";
    batch.trips.push_back(tr);

    batch.run_passengers.push_back(RunPassengerLink{"r1", "t1"});

    CarCallRecord cc;
    cc.call_key            = "c1";
    cc.cabin_id            = 2;
    cc.floor               = 5;
    cc.registered_at_ms    = static_cast<double>(now_ms + 3000);
    cc.registered_stop_key = "s1";
    cc.origin_floor        = 0;
    cc.has_origin_floor    = true;
    cc.cleared_at_ms       = static_cast<double>(now_ms + 21000);
    cc.served_stop_key     = "s2";
    cc.outcome             = "served";
    cc.source              = "button";
    batch.car_calls.push_back(cc);

    HallCallRecord hc;
    hc.call_key          = "h1";
    hc.floor             = 0;
    hc.direction         = "up";
    hc.registered_at_ms  = static_cast<double>(now_ms - 5000);
    hc.cleared_at_ms     = static_cast<double>(now_ms + 1000);
    hc.served_cabin_id   = 2;
    hc.has_served_cabin  = true;
    hc.served_stop_key   = "s1";
    hc.door_opened_at_ms = static_cast<double>(now_ms + 1000);
    hc.wait_ms           = 6000;
    hc.outcome           = "served";
    batch.hall_calls.push_back(hc);

    std::string err;
    CHECK(tdb.insert_batch(batch, &err), "insert batch succeeded");

    // Export CSV with floor labels B, 5.
    auto exp = tdb.export_csv(csv_dir, now_ms - 10000, now_ms + 50000);
    CHECK(exp.ok && exp.row_counts["passenger_trips"] == 1 && exp.row_counts["car_calls"] == 1 &&
              exp.row_counts["hall_calls"] == 1,
          "export CSV wrote expected row counts");
    const std::string trips_csv = slurp_file(csv_dir + "/passenger_trips.csv");
    CHECK(trips_csv.find(",B,5,") != std::string::npos,
          "CSV includes formatted floor labels (B for 0, 5 for 5)");

    // Merge vis2 into vis1.
    CHECK(tdb.merge_visitors(vis2, vis1, now_ms + 30000, 10, err), "merge vis2 into vis1");
    VisitorShowResult show_vis1;
    CHECK(tdb.show_visitor(vis1, show_vis1) && show_vis1.summary.trip_count == 3 &&
              show_vis1.summary.embedding_count == 3,
          "merged visitor combines trip count and embeddings");

    // Prepare resident DB and test suggest + promote.
    {
        ResidentDB rdb;
        CHECK(rdb.open(rdb_path, resident_schema, false), "open temp residents.db");
        ResidentEnrollRequest req;
        req.name       = "Existing Resident";
        req.home_floor = 5;
        req.source     = "id_photo";
        req.embeddings.push_back(make_unit_vec(8, 0));
        auto res = write_resident_enrollment(rdb, req);
        CHECK(res.ok && res.resident_id > 0, "seed resident in residents.db");
        rdb.close();
    }

    std::vector<PromotionSuggestion> suggestions;
    CHECK(tdb.suggest_promotions(rdb_path, 8, 1, 0.7f, suggestions, err) &&
              suggestions.size() == 1 && suggestions[0].visitor_id == vis1,
          "suggest_promotions finds matching active visitor");

    PromoteRequest preq;
    preq.visitor_id           = vis1;
    preq.existing_resident_id = suggestions[0].resident_id;
    preq.resident_db_path     = rdb_path;
    preq.resident_schema_path = resident_schema;
    preq.now_ms               = now_ms + 40000;
    auto pres                 = tdb.promote_visitor(preq);
    CHECK(pres.ok && pres.embeddings_copied == 3 && pres.trips_updated == 1,
          "promote copies visitor embeddings to residents.db and updates passenger_trips");

    VisitorShowResult after_promote;
    CHECK(tdb.show_visitor(vis1, after_promote) && after_promote.summary.status == "promoted" &&
              after_promote.embeddings.empty(),
          "promoted visitor clears visitor_embeddings and sets promoted status");

    // Test delete_visitor & purge_expired.
    std::int64_t vis3 = -1, emb_v3 = -1;
    CHECK(tdb.create_visitor(now_ms, make_unit_vec(8, 3), 0.7f, vis3, emb_v3), "create visitor 3");
    std::vector<std::string> unlink_paths;
    CHECK(tdb.delete_visitor(vis3, now_ms + 50000, unlink_paths, err), "delete visitor 3");
    VisitorShowResult after_del;
    CHECK(tdb.show_visitor(vis3, after_del) && after_del.summary.status == "deleted",
          "deleted visitor status marked deleted");

    std::int64_t vis_old = -1, emb_old = -1;
    CHECK(tdb.create_visitor(now_ms, make_unit_vec(8, 4), 0.6f, vis_old, emb_old),
          "create old visitor");
    TrafficPurgeResult purge_res;
    CHECK(tdb.purge_expired(now_ms + 100LL * 86400000LL, 30, 90, purge_res, &err) &&
              purge_res.rows_deleted >= 5 && purge_res.visitors_expired == 1,
          "purge removes expired traffic rows and expires old visitors");

    tdb.close();
    std::string rm_cmd = "rm -rf '" + base_dir + "'";
    std::system(rm_cmd.c_str());

    std::printf("\n[test_traffic_db] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
