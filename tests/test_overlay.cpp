#include "overlay.h"

#include <cstdio>
#include <cstring>

#include <opencv2/imgproc.hpp>

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

static OccupancyConfig make_config() {
    OccupancyConfig config;
    CountingZone    zone;
    zone.name    = "door";
    zone.polygon = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.75f, 0.75f}, {0.25f, 0.75f}};
    zone.inside_direction_degrees  = 90.0f;
    zone.min_crossing_displacement = 0.05f;
    config.zones.push_back(zone);
    return config;
}

int main() {
    UiScale       ui = UiScale::compute(200, 1.0f);
    cv::Mat       frame(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
    const cv::Mat original = frame.clone();
    draw_counting_zones(frame, OccupancyConfig{}, frame.cols, frame.rows, ui);
    CHECK(cv::norm(frame, original, cv::NORM_INF) == 0.0,
          "disabled/empty zone config does not alter preview");
    draw_counting_zones(frame, make_config(), frame.cols, frame.rows, ui);
    CHECK(cv::norm(frame, original, cv::NORM_INF) > 0.0, "enabled counting zone is drawn");

    char hud[64] = "FPS 10.0";
    append_occupancy_hud(hud, sizeof(hud), false, 4, true);
    CHECK(std::strstr(hud, "OCC") == nullptr, "disabled HUD omits occupancy");
    append_occupancy_hud(hud, sizeof(hud), true, 4, true);
    CHECK(std::strstr(hud, "OCC 4?") != nullptr, "enabled HUD shows count and uncertainty");

    std::printf("\n[test_overlay] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
