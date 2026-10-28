#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "occupancy_counter.h"
#include "video_io.h"

static int checks = 0, failed = 0;
#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

int main() {
    FrameSlot         slot;
    std::atomic<bool> stop{false};
    uint64_t          seq = 0;
    cv::Mat           frame;
    auto              t0 = std::chrono::steady_clock::now();
    auto              r  = wait_for_frame_or_deadline(&slot, &stop, &seq, &frame, 40.0);
    auto              ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK(r == FrameWaitResult::Deadline, "no-frame wait reaches deadline");
    CHECK(ms >= 25 && ms < 300, "deadline wait is timed, not busy or indefinite");

    OccupancyConfig occupancy_config;
    CountingZone    zone;
    zone.name                     = "door";
    zone.polygon                  = {{0.3f, 0.4f}, {0.7f, 0.4f}, {0.7f, 0.6f}, {0.3f, 0.6f}};
    zone.inside_direction_degrees = 90.0f;
    occupancy_config.zones.push_back(zone);
    OccupancyState initial;
    initial.count      = 3;
    initial.confidence = OccupancyConfidence::Certain;
    OccupancyCounter counter(occupancy_config, initial);
    auto             outage = counter.update({}, 0, 0, 40.0, r != FrameWaitResult::Deadline, false);
    CHECK(outage.count == 3 && outage.confidence == OccupancyConfidence::Uncertain,
          "deadline boundary updates outage confidence without resetting count");

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(slot.mtx);
        slot.latest = cv::Mat(2, 3, CV_8UC3, cv::Scalar(7, 8, 9));
        slot.seq++;
        slot.cv_new.notify_one();
    });
    r = wait_for_frame_or_deadline(&slot, &stop, &seq, &frame, 500.0);
    producer.join();
    CHECK(r == FrameWaitResult::Frame && seq == 1, "new frame wakes before deadline");
    CHECK(frame.rows == 2 && frame.cols == 3, "published frame copied");

    stop.store(true);
    r = wait_for_frame_or_deadline(&slot, &stop, &seq, &frame, 500.0);
    CHECK(r == FrameWaitResult::Stop, "stop flag terminates wait");

    std::printf("\n[test_video_wait] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
