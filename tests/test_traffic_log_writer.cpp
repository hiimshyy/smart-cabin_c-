#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "traffic_log_writer.h"

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

FaceSample make_sample(int dim, float x0, float x1, float quality, bool with_crop) {
    FaceSample s;
    s.embedding.assign(static_cast<std::size_t>(dim), 0.0f);
    s.embedding[0] = x0;
    s.embedding[1] = x1;
    float norm     = std::sqrt(x0 * x0 + x1 * x1);
    if (norm > 0.0f) {
        s.embedding[0] /= norm;
        s.embedding[1] /= norm;
    }
    s.quality = quality;
    if (with_crop) {
        s.crop_w = 112;
        s.crop_h = 112;
        s.crop_bgr_112.assign(112 * 112 * 3, 128);
    }
    return s;
}

int count_table_rows(const std::string& db_path, const char* table) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return -1;
    sqlite3_stmt*     st  = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table + ";";
    int               cnt = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) cnt = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return cnt;
}

std::string first_face_image(const std::string& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return {};
    sqlite3_stmt* st = nullptr;
    std::string   res;
    if (sqlite3_prepare_v2(
            db, "SELECT face_image FROM passenger_trips WHERE face_image IS NOT NULL LIMIT 1;", -1,
            &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* s = sqlite3_column_text(st, 0);
            if (s) res = reinterpret_cast<const char*>(s);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return res;
}

void test_queue_overflow_and_flush_on_close(const std::string& schema_path,
                                            const std::string& base_dir) {
    const std::string db_path = base_dir + "/overflow.db";

    TrafficLogWriterConfig cfg;
    cfg.db_path         = db_path;
    cfg.schema_sql_path = schema_path;
    cfg.max_queue       = 4;
    cfg.max_batch       = 2;
    cfg.recog_dim       = 4;
    cfg.steady_clock_fn = []() { return 1000.0; };
    cfg.system_clock_fn = []() { return 1700000000000.0; };
    cfg.free_space_fn   = [](const std::string&, std::uint64_t& free_bytes) {
        free_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
        return true;
    };

    TrafficLogWriter writer(cfg);
    CHECK(writer.start(), "writer starts cleanly for overflow test");

    // Push 10 trips in a single call (> max_queue=4) so drop-oldest fires immediately.
    TripAssemblerOutput out;
    for (int i = 0; i < 10; ++i) {
        PassengerTripRecord tr;
        tr.trip_key   = "ov-" + std::to_string(i);
        tr.cabin_id   = 2;
        tr.confidence = "high";
        out.completed_trips.push_back(std::move(tr));
    }
    writer.enqueue_assembler_output(out);
    CHECK(writer.dropped_count() == 6, "bounded queue drops oldest 6 of 10 records when cap=4");

    writer.close();
    CHECK(count_table_rows(db_path, "passenger_trips") == 4,
          "close flushes surviving 4 queued trips to sqlite");
}

void test_disk_guard_and_face_jpeg_permissions(const std::string& schema_path,
                                               const std::string& base_dir) {
    const std::string db_path = base_dir + "/guard.db";

    // Case A: free space between 1024 MB and 4096 MB (2048 MB) -> DB writes proceed, JPEGs paused.
    std::uint64_t simulated_free_mb = 2048;

    TrafficLogWriterConfig cfg;
    cfg.db_path                = db_path;
    cfg.schema_sql_path        = schema_path;
    cfg.recog_dim              = 4;
    cfg.visitor_reid           = true;
    cfg.save_faces             = TrafficSaveFaces::Visitors;
    cfg.min_free_mb            = 4096;
    cfg.critical_free_mb       = 1024;
    cfg.disk_check_interval_ms = 0.0; // re-check every batch for deterministic test
    cfg.steady_clock_fn        = []() { return 5000.0; };
    cfg.system_clock_fn        = []() { return 1700000005000.0; };
    cfg.free_space_fn          = [&](const std::string&, std::uint64_t& free_bytes) {
        free_bytes = simulated_free_mb * 1024ULL * 1024ULL;
        return true;
    };

    TrafficLogWriter writer(cfg);
    CHECK(writer.start(), "writer starts cleanly for disk guard test");

    TripAssemblerOutput out1;
    PassengerTripRecord tr1;
    tr1.trip_key      = "tr-lowdisk";
    tr1.cabin_id      = 2;
    tr1.entered_at_ms = 5000.0;
    tr1.exited_at_ms  = 5000.0;
    tr1.identity_kind = "visitor";
    tr1.confidence    = "high";
    tr1.face_samples.push_back(make_sample(4, 1.0f, 0.0f, 0.8f, true));
    tr1.face_samples.push_back(make_sample(4, 0.99f, 0.05f, 0.9f, true));
    out1.completed_trips.push_back(std::move(tr1));

    CallStepResult calls;
    CarCallRecord  cc;
    cc.call_key = "cc-1";
    cc.cabin_id = 2;
    cc.floor    = 5;
    cc.outcome  = "served";
    cc.source   = "button";
    calls.completed_car_calls.push_back(cc);

    writer.enqueue_assembler_output(out1);
    writer.enqueue_calls(calls);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    CHECK(writer.face_images_saved() == 0,
          "face JPEG saving paused when free space < min_free_mb (4096)");

    // Case B: abundant free space (8192 MB) -> visitor resolved + JPEG written with mode 0600.
    simulated_free_mb = 8192;
    TripAssemblerOutput out2;
    PassengerTripRecord tr2;
    tr2.trip_key      = "tr-okdisk";
    tr2.cabin_id      = 2;
    tr2.entered_at_ms = 5000.0;
    tr2.exited_at_ms  = 5000.0;
    tr2.identity_kind = "visitor";
    tr2.confidence    = "high";
    tr2.face_samples.push_back(make_sample(4, 1.0f, 0.01f, 0.95f, true));
    tr2.face_samples.push_back(make_sample(4, 0.99f, 0.02f, 0.85f, false));
    out2.completed_trips.push_back(std::move(tr2));
    writer.enqueue_assembler_output(out2);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Case C: critically low space (512 MB < 1024 MB) -> DB writes paused cleanly.
    simulated_free_mb = 512;
    TripAssemblerOutput out3;
    PassengerTripRecord tr3;
    tr3.trip_key   = "tr-critdisk";
    tr3.cabin_id   = 2;
    tr3.confidence = "high";
    out3.completed_trips.push_back(std::move(tr3));
    writer.enqueue_assembler_output(out3);

    writer.close();

    CHECK(count_table_rows(db_path, "passenger_trips") == 2,
          "critically low disk (<1024 MB) skips new DB writes while keeping earlier 2 trips");
    CHECK(count_table_rows(db_path, "car_calls") == 1, "car_calls flushed cleanly");
    CHECK(count_table_rows(db_path, "visitors") == 1 && writer.visitors_active() == 1,
          "two trips of same visitor resolve to single active visitor");

    const std::string jpeg_path = first_face_image(db_path);
    struct stat       st {};
    CHECK(!jpeg_path.empty() && ::stat(jpeg_path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "saved 112x112 face JPEG exists with 0600 permissions");
}

} // namespace

int main(int argc, char** argv) {
    const std::string schema_path =
        (argc >= 2) ? argv[1] : "/home/orangepi/face_recog_app/db/traffic_schema.sql";
    const std::string base_dir =
        "/tmp/scabin_tests/traffic_writer_test_" + std::to_string(::getpid());

    test_queue_overflow_and_flush_on_close(schema_path, base_dir);
    test_disk_guard_and_face_jpeg_permissions(schema_path, base_dir);

    const std::string rm_cmd = "rm -rf '" + base_dir + "'";
    std::system(rm_cmd.c_str());

    std::printf("\n[test_traffic_log_writer] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
