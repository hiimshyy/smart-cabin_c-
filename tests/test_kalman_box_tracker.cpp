#include <cmath>
#include <cstdio>

#include "kalman_box_tracker.h"

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

int main() {
    KalmanBoxTracker tracker({0, 0, 100, 200}, 0.0);
    tracker.predict(100.0, 2000.0);
    tracker.correct({10, 0, 110, 200}, 100.0);
    tracker.predict(200.0, 2000.0);
    BoxF moving = tracker.box();
    CHECK(moving.x1 > 10.0f, "constant velocity prediction advances box");
    CHECK(moving.x2 > moving.x1 && moving.y2 > moving.y1, "predicted box remains positive");

    tracker.predict(240.0, 2000.0);
    BoxF short_step = tracker.box();
    CHECK(short_step.x1 >= moving.x1, "variable positive dt stays monotonic");

    tracker.predict(100.0, 2000.0);
    BoxF backwards = tracker.box();
    CHECK(std::isfinite(backwards.x1) && backwards.x2 > backwards.x1,
          "backwards timestamp is safe");

    tracker.predict(5000.0, 1000.0);
    BoxF after_gap = tracker.box();
    CHECK(std::isfinite(after_gap.x1) && after_gap.x2 > after_gap.x1,
          "large reconnect gap resets safely");

    tracker.correct({50, 30, 50, 30}, 5100.0);
    BoxF invalid_measurement = tracker.box();
    CHECK(invalid_measurement.x2 > invalid_measurement.x1 && invalid_measurement.y2 > invalid_measurement.y1,
          "invalid measurement is clamped positive");

    std::printf("\n[test_kalman_box_tracker] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
