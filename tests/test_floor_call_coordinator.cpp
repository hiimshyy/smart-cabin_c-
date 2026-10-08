#include "floor_call_coordinator.h"

#include <cstdio>
#include <vector>

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s\n", m); } } while (0)

int main() {
    std::vector<FloorCallCommand> queued;
    std::vector<FloorCallDecision> decisions;
    FloorCallConfig config;
    config.cabin_id = 2;
    config.floors_min = 1;
    config.floors_max = 30;
    config.session_empty_ms = 5000;
    config.max_commands_per_visit = 4;
    FloorCallCoordinator coordinator(config, [&queued](const FloorCallCommand& command) {
        queued.push_back(command);
        return true;
    });
    coordinator.set_decision_callback([&decisions](const FloorCallDecision& decision) {
        decisions.push_back(decision);
    });

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
    CHECK(same_floor.state == FloorCallState::Suppressed &&
          same_floor.reason == "same_floor" && queued.size() == 1,
          "two residents on one floor create one command");

    auto other_floor = coordinator.on_confirmed(23, 300, 6, 0.89f, 40.0);
    CHECK(other_floor.state == FloorCallState::Queued && queued.size() == 2 &&
          queued[1].value == 64, "different floor queues its own bit value");

    auto no_floor = coordinator.on_confirmed(24, 400, 0, 0.90f, 50.0);
    CHECK(no_floor.state == FloorCallState::Suppressed && no_floor.reason == "no_floor",
          "resident without floor does not queue a command");

    auto out_of_range = coordinator.on_confirmed(25, 500, 31, 0.90f, 60.0);
    CHECK(out_of_range.state == FloorCallState::Rejected &&
          out_of_range.reason == "floor_out_of_cabin_range", "invalid cabin floor is rejected");

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
          queued.back().visit_id == 2, "resident can call again in a new cabin visit");

    std::printf("[test_floor_call_coordinator] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
