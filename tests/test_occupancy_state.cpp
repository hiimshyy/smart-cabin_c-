#include "occupancy_state.h"

#include "log/logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

static int checks = 0;
static int failed = 0;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", message, __LINE__);                              \
        }                                                                                          \
    } while (0)

static const fs::path& temp_dir() {
    static const fs::path path =
        fs::temp_directory_path() / ("occupancy-state-" + std::to_string(getpid()));
    static const bool created = fs::create_directories(path);
    (void)created;
    return path;
}

static std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static size_t count_temporary_files(const fs::path& target) {
    const std::string prefix = target.filename().string() + ".tmp.";
    size_t            count  = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(target.parent_path())) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

static void test_missing_corrupt_and_warning() {
    std::printf("test_missing_corrupt_and_warning\n");
    FILE*     capture      = std::tmpfile();
    const int saved_stderr = dup(fileno(stderr));
    std::fflush(stderr);
    dup2(fileno(capture), fileno(stderr));
    LogConfig config;
    config.to_stderr = true;
    config.to_file   = false;
    config.color     = false;
    Logger::instance().init(config);

    std::string          error;
    OccupancyLoadStatus  status = OccupancyLoadStatus::Loaded;
    OccupancyStateHolder missing((temp_dir() / "missing.json").string());
    CHECK(!missing.load(error, &status) && status == OccupancyLoadStatus::Missing,
          "missing state is observable");
    CHECK(missing.get().count == 0 && missing.get().confidence == OccupancyConfidence::Uncertain,
          "missing state resets to zero uncertain");

    const fs::path corrupt_path = temp_dir() / "corrupt.json";
    std::ofstream(corrupt_path) << R"({"version":1,"count":9.5})";
    OccupancyStateHolder corrupt(corrupt_path.string());
    CHECK(!corrupt.load(error, &status) && status == OccupancyLoadStatus::Invalid,
          "corrupt state is observable");
    CHECK(corrupt.get().count == 0 && corrupt.get().confidence == OccupancyConfidence::Uncertain,
          "corrupt state resets count instead of retaining partial data");

    Logger::instance().flush();
    std::fflush(stderr);
    dup2(saved_stderr, fileno(stderr));
    close(saved_stderr);
    std::rewind(capture);
    std::string log;
    char        buffer[512];
    while (std::fgets(buffer, sizeof(buffer), capture))
        log += buffer;
    std::fclose(capture);
    Logger::instance().shutdown();
    CHECK(log.find("state file missing") != std::string::npos,
          "missing state emits application warning");
    CHECK(log.find("state file invalid") != std::string::npos,
          "corrupt state emits application warning");
}

static void test_round_trip_and_atomic_preservation() {
    std::printf("test_round_trip_and_atomic_preservation\n");
    const fs::path path = temp_dir() / "round-trip.json";
    std::string    error;
    {
        OccupancyStateHolder holder(path.string());
        holder.set(3, OccupancyConfidence::Certain);
        CHECK(holder.save(error), "synchronous save succeeds");
    }
    OccupancyStateHolder loaded(path.string());
    CHECK(loaded.load(error), "saved state loads");
    const OccupancyState state = loaded.get();
    CHECK(state.count == 3 && state.confidence == OccupancyConfidence::Certain,
          "count and confidence round trip");
    CHECK(state.generation == 1 && state.saved_at_ms > 1000000000000.0,
          "generation and wall-clock timestamp round trip");

    const fs::path preserved = temp_dir() / "preserved.json";
    {
        OccupancyStateHolder holder(preserved.string());
        holder.set(5, OccupancyConfidence::Certain);
        CHECK(holder.save(error), "preserved target initial save");
    }
    const std::string            before     = read_file(preserved);
    const AtomicSaveFailurePoint failures[] = {
        AtomicSaveFailurePoint::TempOpen,      AtomicSaveFailurePoint::Write,
        AtomicSaveFailurePoint::FileFsync,     AtomicSaveFailurePoint::Rename,
        AtomicSaveFailurePoint::DirectoryOpen,
    };
    OccupancyStateHolder replacement(preserved.string());
    replacement.set(99, OccupancyConfidence::Certain);
    for (AtomicSaveFailurePoint failure : failures) {
        set_occupancy_state_save_failure_for_test(failure);
        CHECK(!replacement.save(error), "injected state I/O failure is reported");
        CHECK(read_file(preserved) == before,
              "pre-rename state failure preserves same target byte-for-byte");
        CHECK(count_temporary_files(preserved) == 0, "failed state save cleans temporary file");
    }
    set_occupancy_state_save_failure_for_test(AtomicSaveFailurePoint::None);

    set_occupancy_state_save_failure_for_test(AtomicSaveFailurePoint::DirectoryFsync);
    CHECK(!replacement.save(error), "post-rename state directory fsync failure is reported");
    set_occupancy_state_save_failure_for_test(AtomicSaveFailurePoint::None);
    CHECK(count_temporary_files(preserved) == 0,
          "directory fsync failure leaves no temporary state file");
    OccupancyStateHolder preserved_load(preserved.string());
    CHECK(preserved_load.load(error) && preserved_load.get().count == 99,
          "directory fsync failure occurs only after atomic replacement");
}

static void test_bounded_throttled_worker_and_close_flush() {
    std::printf("test_bounded_throttled_worker_and_close_flush\n");
    const fs::path       path = temp_dir() / "async.json";
    std::string          error;
    OccupancyStateHolder holder(path.string());
    holder.request_save(1, OccupancyConfidence::Certain);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fs::exists(path) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fs::exists(path), "first async snapshot is persisted");
    holder.request_save(2, OccupancyConfidence::Uncertain);
    holder.request_save(7, OccupancyConfidence::Certain);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    OccupancyStateHolder early(path.string());
    CHECK(early.load(error) && early.get().count == 1,
          "burst does not perform a second write inside one second");
    early.close();

    const auto close_start = std::chrono::steady_clock::now();
    holder.close();
    const auto close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - close_start)
                              .count();
    CHECK(close_ms >= 650, "shutdown flush respects one-write-per-second throttle");

    OccupancyStateHolder final_state(path.string());
    CHECK(final_state.load(error), "flushed state loads");
    CHECK(final_state.get().count == 7 &&
              final_state.get().confidence == OccupancyConfidence::Certain,
          "bounded pending slot keeps and flushes newest snapshot");
    CHECK(final_state.get().generation == 3, "coalesced changes retain generation");
}

int main() {
    test_missing_corrupt_and_warning();
    test_round_trip_and_atomic_preservation();
    test_bounded_throttled_worker_and_close_flush();
    fs::remove_all(temp_dir());
    std::printf("\n[test_occupancy_state] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
