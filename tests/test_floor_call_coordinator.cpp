#include "floor_call_coordinator.h"

#include <cstdio>
#include <vector>

static int checks = 0;
static int failed = 0;
#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s\n", m);                                                        \
        }                                                                                          \
    } while (0)

int main() {
    std::vector<FloorCallCommand>  queued;
    std::vector<FloorCallDecision> decisions;
    FloorCallConfig                config;
    config.cabin_id               = 2;
    config.floors_min             = 0;
    config.floors_max             = 30;
    config.session_empty_ms       = 5000;
    config.max_commands_per_visit = 4;
    FloorCallCoordinator coordinator(config, [&queued](const FloorCallCommand& command) {
        queued.push_back(command);
        return true;
    });
    coordinator.set_decision_callback(
        [&decisions](const FloorCallDecision& decision) { decisions.push_back(decision); });

    coordinator.on_presence({10}, 0.0);
    CHECK(coordinator.active_visit_id() == 1, "first present person starts visit 1");

    auto first = coordinator.on_confirmed(10, 100, 5, 0.91f, 10.0);
    CHECK(first.state == FloorCallState::Queued && queued.size() == 1,
          "resident 100 floor 5 queues one command");
    CHECK(queued[0].value == 32 && queued[0].request_id == "ai-core:2:1:1",
          "command carries value 32 and unique request id");

    auto same_resident = coordinator.on_confirmed(21, 100, 5, 0.92f, 20.0);
    CHECK(same_resident.state == FloorCallState::Suppressed &&
              same_resident.reason == "same_resident" && queued.size() == 1,
          "track change cannot call the same resident again");

    auto same_floor = coordinator.on_confirmed(22, 200, 5, 0.90f, 30.0);
    CHECK(same_floor.state == FloorCallState::Suppressed && same_floor.reason == "same_floor" &&
              queued.size() == 1,
          "two residents on one floor create one command");

    auto other_floor = coordinator.on_confirmed(23, 300, 6, 0.89f, 40.0);
    CHECK(other_floor.state == FloorCallState::Queued && queued.size() == 2 &&
              queued[1].value == 64,
          "different floor queues its own bit value");

    auto no_floor = coordinator.on_confirmed(24, 400, -1, 0.90f, 50.0);
    CHECK(no_floor.state == FloorCallState::Suppressed && no_floor.reason == "no_floor",
          "resident without floor (-1) does not queue a command");

    auto out_of_range = coordinator.on_confirmed(25, 500, 31, 0.90f, 60.0);
    CHECK(out_of_range.state == FloorCallState::Rejected &&
              out_of_range.reason == "floor_out_of_cabin_range",
          "invalid cabin floor is rejected");

    coordinator.on_transport_state(first.request_id, FloorCallState::Sent);
    coordinator.on_transport_state(first.request_id, FloorCallState::AckedSuccess, "OK");
    CHECK(!decisions.empty() && decisions.back().state == FloorCallState::AckedSuccess,
          "transport success is recorded after send");

    coordinator.on_presence({}, 1000.0);
    coordinator.on_presence({}, 5999.0);
    CHECK(coordinator.active_visit_id() == 1, "short empty interval keeps visit open");
    coordinator.on_presence({}, 6001.0);
    CHECK(coordinator.active_visit_id() == 0, "empty timeout closes visit 1");
    coordinator.on_presence({31}, 6002.0);
    CHECK(coordinator.active_visit_id() == 2, "new presence uses next visit id");
    auto reentry = coordinator.on_confirmed(31, 100, 5, 0.93f, 6010.0);
    CHECK(reentry.state == FloorCallState::Queued && queued.size() == 3 &&
              queued.back().visit_id == 2,
          "resident can call again in a new cabin visit");

    // ---- secondary_floor selection rules ----
    auto ctx = [](int floor, CabinTravelDirection dir) {
        CabinFloorContext c;
        c.has_current_floor = floor >= 0;
        c.current_floor     = floor;
        c.direction         = dir;
        return c;
    };

    // Unknown cabin floor falls back to home_floor (or no_floor if only secondary set).
    {
        auto r = FloorCallCoordinator::select_target_floor(5, 1, CabinFloorContext{});
        CHECK(r.target_floor == 5 && r.reason.empty(), "unknown cabin floor uses home_floor");
        auto r2 = FloorCallCoordinator::select_target_floor(-1, 1, CabinFloorContext{});
        CHECK(r2.target_floor == -1 && r2.reason == "no_floor",
              "unknown cabin floor without home_floor returns no_floor");
    }

    // (home=5, secondary=1): from >=5 goes down to 1; from <=1 goes up to 5.
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(5, CabinTravelDirection::Unknown))
                  .target_floor == 1,
          "home=5,sec=1 at floor 5 calls 1");
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(6, CabinTravelDirection::Up))
                  .target_floor == 1,
          "home=5,sec=1 at floor 6 calls 1");
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(1, CabinTravelDirection::Unknown))
                  .target_floor == 5,
          "home=5,sec=1 at floor 1 calls 5");
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(0, CabinTravelDirection::Up))
                  .target_floor == 5,
          "home=5,sec=1 at floor B calls 5");
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(3, CabinTravelDirection::Down))
                  .target_floor == 1,
          "home=5,sec=1 at intermediate floor 3 going down calls 1");
    CHECK(FloorCallCoordinator::select_target_floor(5, 1, ctx(3, CabinTravelDirection::Up))
                  .target_floor == 5,
          "home=5,sec=1 at intermediate floor 3 going up calls 5");

    // (home=3, secondary=0/'B'): intermediate floor 1 vs 2 + direction tie-break.
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(3, CabinTravelDirection::Unknown))
                  .target_floor == 0,
          "home=3,sec=B at floor 3 calls B (0)");
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(1, CabinTravelDirection::Up))
                  .target_floor == 3,
          "home=3,sec=B at floor 1 going up calls 3");
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(1, CabinTravelDirection::Unknown))
                  .target_floor == 3,
          "home=3,sec=B at floor 1 idle calls 3 (cur <= 1 goes up)");
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(1, CabinTravelDirection::Down))
                  .target_floor == 0,
          "home=3,sec=B at floor 1 going down calls B (0)");
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(2, CabinTravelDirection::Down))
                  .target_floor == 0,
          "home=3,sec=B at floor 2 going down calls B (0)");
    CHECK(FloorCallCoordinator::select_target_floor(3, 0, ctx(2, CabinTravelDirection::Unknown))
                  .target_floor == 0,
          "home=3,sec=B at floor 2 idle calls B (0)");

    // No secondary_floor when cabin is already at or above home_floor.
    {
        auto at_home =
            FloorCallCoordinator::select_target_floor(3, -1, ctx(3, CabinTravelDirection::Unknown));
        CHECK(at_home.target_floor == -1 && at_home.reason == "already_at_floor",
              "home=3,sec=-1 at floor 3 is suppressed (already_at_floor)");
        auto above_home =
            FloorCallCoordinator::select_target_floor(3, -1, ctx(5, CabinTravelDirection::Down));
        CHECK(above_home.target_floor == -1 && above_home.reason == "no_secondary_floor",
              "home=3,sec=-1 at floor 5 is suppressed (no_secondary_floor)");
    }

    // CabinFloorTracker hall-call memory overrides stale 8480 direction.
    {
        CabinFloorTracker tracker;
        // Floor 3 has HALL_CALLS_DOWN (1<<3 = 8) while 8480 still reports up=true.
        tracker.update(true, 3, /*up=*/true, /*down=*/false,
                       /*hall_up=*/0, true, /*hall_down=*/(1u << 3), true, 1000.0);
        auto s1 = tracker.snapshot(1200.0);
        CHECK(s1.has_current_floor && s1.current_floor == 3 &&
                  s1.direction == CabinTravelDirection::Down,
              "hall-call down overrides stale 8480 up bit");
        // Hall call clears on door open; memory lasts for kHallCallMemoryMs.
        tracker.update(true, 3, /*up=*/true, /*down=*/false,
                       /*hall_up=*/0, true, /*hall_down=*/0, true, 2000.0);
        auto s2 = tracker.snapshot(5000.0);
        CHECK(s2.direction == CabinTravelDirection::Down,
              "hall-call memory preserves Down within 15s window");
        auto s3 = tracker.snapshot(20000.0);
        CHECK(s3.direction == CabinTravelDirection::Up,
              "expired hall-call memory falls back to 8480 status");
        tracker.reset();
        auto s4 = tracker.snapshot(21000.0);
        CHECK(!s4.has_current_floor && s4.current_floor == -1 &&
                  s4.direction == CabinTravelDirection::Unknown,
              "tracker reset clears floor context on disconnect");
    }

    // End-to-end 7-arg on_confirmed queuing floor 0 ('B') -> value 1, and suppressing same
    // resident.
    {
        coordinator.on_presence({}, 12000.0); // close visit 2
        coordinator.on_presence({40}, 12100.0);
        const size_t before = queued.size();
        auto d_b = coordinator.on_confirmed(40, 600, 3, 0, ctx(3, CabinTravelDirection::Unknown),
                                            0.95f, 12110.0);
        CHECK(d_b.state == FloorCallState::Queued && d_b.floor == 0 &&
                  queued.size() == before + 1 && queued.back().floor == 0 &&
                  queued.back().value == 1,
              "resident (home=3,sec=B) at floor 3 queues Basement 0 with value=1");
        auto d_s1 = coordinator.on_confirmed(41, 601, 5, 1, ctx(5, CabinTravelDirection::Unknown),
                                             0.94f, 12120.0);
        CHECK(d_s1.state == FloorCallState::Queued && d_s1.floor == 1 && queued.back().value == 2,
              "resident (home=5,sec=1) at floor 5 queues floor 1 with value=2");
        auto d_sup = coordinator.on_confirmed(42, 602, 3, -1, ctx(3, CabinTravelDirection::Unknown),
                                              0.94f, 12130.0);
        CHECK(d_sup.state == FloorCallState::Suppressed && d_sup.reason == "already_at_floor" &&
                  queued.size() == before + 2,
              "resident without secondary_floor at home_floor is suppressed without queuing");
    }

    std::printf("[test_floor_call_coordinator] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
