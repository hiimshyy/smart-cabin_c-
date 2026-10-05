#include <cstdio>

#include "tracker_greedy.h"

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static PersonDet person(float x, float score) {
    return {x, 10.0f, x + 80.0f, 210.0f, score};
}

int main() {
    TrackerConfig config;
    config.high_threshold = 0.5f;
    GreedyIouTracker tracker(config, 30, 90);

    for (int frame = 0; frame < 3; ++frame)
        tracker.update({person(10.0f, 0.2f)}, {frame, frame * 33.0, 640, 480});
    CHECK(tracker.active_count() == 0, "low confidence detections cannot create greedy tracks");

    for (int frame = 3; frame < 6; ++frame)
        tracker.update({person(10.0f, 0.9f)}, {frame, frame * 33.0, 640, 480});
    CHECK(tracker.active_count() == 1 && tracker.tracked_count() == 1,
          "high confidence detections confirm one greedy track");

    tracker.update({person(10.0f, 0.9f), person(250.0f, 0.2f)}, {6, 198.0, 640, 480});
    CHECK(tracker.active_count() == 1,
          "a simultaneous low confidence false positive cannot add a greedy track");

    GreedyIouTracker cabin(config, 30, 90);
    for (int frame = 0; frame < 3; ++frame) {
        cabin.update({person(10.0f, 0.9f), person(130.0f, 0.85f),
                      person(250.0f, 0.8f), person(370.0f, 0.25f),
                      person(490.0f, 0.15f)}, {frame, frame * 33.0, 640, 480});
    }
    CHECK(cabin.tracked_count() == 3 && cabin.active_count() == 3,
          "three high detections and two low false positives produce three greedy tracks");

    std::printf("\n[test_greedy_adapter] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
