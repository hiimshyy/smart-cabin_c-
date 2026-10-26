#include "door_inference.h"

#include <cstdio>

static int checks = 0, failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL: %s\n", m); } } while (0)

int main() {
    DoorInferenceScheduler scheduler;
    auto initial_unknown = scheduler.plan(0, decode_door_register(0, true), {});
    CHECK(initial_unknown.state == DoorInferenceState::Degraded && initial_unknown.run_person,
          "zero door bits before a known transition use fallback scan");
    const auto closed = decode_door_register(1u << 6, true);
    auto plan = scheduler.plan(1, closed, {});
    CHECK(plan.state == DoorInferenceState::Idle && !plan.run_person && !plan.run_face,
          "closed empty cabin skips every model");

    const auto open = decode_door_register(1u << 5, true);
    plan = scheduler.plan(10, open, {});
    CHECK(plan.state == DoorInferenceState::DoorOpen && plan.run_person && !plan.run_face,
          "door opening wakes YOLO immediately without SCRFD");
    CHECK(scheduler.face_after_person(10, {1, 1, 1}),
          "newly confirmed person permits SCRFD in the same frame");
    plan = scheduler.plan(30, open, {1, 1, 1});
    CHECK(!plan.run_person && !plan.run_face, "deadlines cap repeated NPU runs");
    plan = scheduler.plan(110, open, {1, 1, 1});
    CHECK(plan.run_person && plan.run_face, "active person runs both stages when due");
    plan = scheduler.plan(220, open, {1, 1, 0});
    CHECK(!plan.run_face, "confirmed identity stops face detection");

    plan = scheduler.plan(300, closed, {1, 1, 0});
    CHECK(plan.state == DoorInferenceState::Occupied && plan.run_person,
          "closing door retains person tracking while occupied");
    plan = scheduler.plan(500, closed, {});
    CHECK(plan.state == DoorInferenceState::EmptyGrace && plan.run_person,
          "empty grace watches for a returning passenger");
    plan = scheduler.plan(3400, closed, {});
    CHECK(plan.state == DoorInferenceState::Idle && !plan.run_person,
          "idle after grace skips YOLO");

    plan = scheduler.plan(3500, decode_door_register(0, false), {});
    CHECK(plan.state == DoorInferenceState::Degraded && plan.run_person,
          "lost door data starts sparse fallback scan");
    CHECK(!decode_door_register((1u << 5) | (1u << 6), true).fresh,
          "contradictory door bits are not accepted as fresh");
    CHECK(scheduler.face_after_person(3500, {1, 1, 1}),
          "fallback scan finding a person runs face detection immediately");
    plan = scheduler.plan(3600, decode_door_register(0, false), {1, 1, 1});
    CHECK(plan.run_person && plan.run_face,
          "unresolved person accelerates fallback YOLO cadence");

    std::printf("[test_door_inference] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
