#include "occupancy_config.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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
        fs::temp_directory_path() / ("occupancy-config-" + std::to_string(getpid()));
    static const bool created = fs::create_directories(path);
    (void)created;
    return path;
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
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

static OccupancyConfig make_config() {
    OccupancyConfig config;
    CountingZone    zone;
    zone.name    = "door";
    zone.polygon = {{0.25f, 0.40f}, {0.75f, 0.40f}, {0.75f, 0.60f}, {0.25f, 0.60f}};
    zone.inside_direction_degrees  = 90.0f;
    zone.min_crossing_displacement = 0.05f;
    config.zones.push_back(std::move(zone));
    return config;
}

static void test_round_trip_and_pixels() {
    std::printf("test_round_trip_and_pixels\n");
    const OccupancyConfig config = make_config();
    const fs::path        path   = temp_dir() / "round-trip.json";
    std::string           error;
    CHECK(save_occupancy_config(path.string(), config, error), "save valid config");

    OccupancyConfig loaded;
    CHECK(load_occupancy_config(path.string(), loaded, error), "load valid config");
    CHECK(loaded.version == 1 && loaded.reference_width == 640 && loaded.reference_height == 480,
          "top-level fields round trip");
    CHECK(loaded.zones.size() == 1 && loaded.zones.front().polygon.size() == 4,
          "single zone round trips");
    const auto pixels = zone_to_pixels(loaded.zones.front(), 640, 480);
    CHECK(pixels.size() == 4 && pixels[0] == cv::Point(160, 192) &&
              pixels[2] == cv::Point(480, 288),
          "normalized polygon scales to frame");
}

static void test_strict_validation() {
    std::printf("test_strict_validation\n");
    std::string       error;
    OccupancyConfig   loaded;
    const std::string prefix =
        R"({"version":1,"reference_width":640,"reference_height":480,"zones":[{"name":"z","polygon":[{"x":0.1,"y":0.1},{"x":0.9,"y":0.1},{"x":0.9,"y":0.9}],"inside_direction_degrees":)";
    const std::string suffix = R"(,"min_crossing_displacement":0}]})";

    write_file(temp_dir() / "bad-version.json",
               R"({"version":9,"reference_width":640,"reference_height":480,"zones":[]})");
    CHECK(!load_occupancy_config((temp_dir() / "bad-version.json").string(), loaded, error),
          "unsupported version rejected");
    write_file(temp_dir() / "fraction-width.json",
               R"({"version":1,"reference_width":640.5,"reference_height":480,"zones":[]})");
    CHECK(!load_occupancy_config((temp_dir() / "fraction-width.json").string(), loaded, error),
          "fractional integer field rejected");
    write_file(temp_dir() / "overflow.json",
               R"({"version":1e999,"reference_width":640,"reference_height":480,"zones":[]})");
    CHECK(!load_occupancy_config((temp_dir() / "overflow.json").string(), loaded, error),
          "out-of-range number rejected without exception");
    write_file(temp_dir() / "malformed-number.json",
               R"({"version":1e,"reference_width":640,"reference_height":480,"zones":[]})");
    CHECK(!load_occupancy_config((temp_dir() / "malformed-number.json").string(), loaded, error),
          "malformed exponent rejected");
    write_file(temp_dir() / "bad-direction.json", prefix + "360" + suffix);
    CHECK(!load_occupancy_config((temp_dir() / "bad-direction.json").string(), loaded, error),
          "direction outside contract rejected");

    OccupancyConfig config = make_config();
    config.zones.push_back(config.zones.front());
    CHECK(!validate_occupancy_config(config, error) &&
              error.find("exactly one") != std::string::npos,
          "v1 explicitly rejects multiple zones");
    config                       = make_config();
    config.zones.front().polygon = {{0.1f, 0.1f}, {0.2f, 0.2f}, {0.3f, 0.3f}};
    CHECK(!validate_occupancy_config(config, error), "zero-area polygon rejected");
    config                                        = make_config();
    config.zones.front().inside_direction_degrees = std::nanf("");
    CHECK(!validate_occupancy_config(config, error), "non-finite direction rejected");
}

static void test_failed_save_preserves_target() {
    std::printf("test_failed_save_preserves_target\n");
    const fs::path  path = temp_dir() / "preserved.json";
    std::string     error;
    OccupancyConfig config = make_config();
    CHECK(save_occupancy_config(path.string(), config, error), "initial save succeeds");
    const std::string before                       = read_file(path);
    config.zones.front().min_crossing_displacement = 0.25f;

    const AtomicSaveFailurePoint failures[] = {
        AtomicSaveFailurePoint::TempOpen,      AtomicSaveFailurePoint::Write,
        AtomicSaveFailurePoint::FileFsync,     AtomicSaveFailurePoint::Rename,
        AtomicSaveFailurePoint::DirectoryOpen,
    };
    for (AtomicSaveFailurePoint failure : failures) {
        set_occupancy_config_save_failure_for_test(failure);
        CHECK(!save_occupancy_config(path.string(), config, error),
              "injected config I/O failure is reported");
        CHECK(read_file(path) == before,
              "pre-rename config failure preserves same target byte-for-byte");
        CHECK(count_temporary_files(path) == 0, "failed config save cleans temporary file");
    }
    set_occupancy_config_save_failure_for_test(AtomicSaveFailurePoint::None);

    set_occupancy_config_save_failure_for_test(AtomicSaveFailurePoint::DirectoryFsync);
    CHECK(!save_occupancy_config(path.string(), config, error),
          "post-rename config directory fsync failure is reported");
    set_occupancy_config_save_failure_for_test(AtomicSaveFailurePoint::None);
    CHECK(count_temporary_files(path) == 0, "directory fsync failure leaves no temporary config");

    config.zones.push_back(config.zones.front());
    const std::string valid_replacement = read_file(path);
    CHECK(!save_occupancy_config(path.string(), config, error), "invalid replacement fails");
    CHECK(read_file(path) == valid_replacement,
          "validation failure does not clobber valid config target");
}

int main() {
    test_round_trip_and_pixels();
    test_strict_validation();
    test_failed_save_preserves_target();
    fs::remove_all(temp_dir());
    std::printf("\n[test_occupancy_config] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
