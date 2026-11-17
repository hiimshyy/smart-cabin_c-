#include "elevator_state.h"

#include "json_minimal.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace jm = json_minimal;

static int checks = 0, failed = 0;
#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

namespace {

RegisterUpdate make_full(std::uint64_t session, double t_ms, int floor_1based, bool running,
                         std::uint16_t r8471, std::uint16_t r8472, std::uint32_t run_cnt,
                         std::uint16_t extra_8480 = 0) {
    RegisterUpdate u;
    u.session     = session;
    u.received_ms = t_ms;
    u.addr_slave  = 2;
    const std::uint16_t r8480 =
        static_cast<std::uint16_t>((running ? (1u << 2) : 0u) | (1u << 0) | extra_8480);
    u.values = {
        {ElevatorReg::FLOOR, 2, static_cast<std::uint16_t>(floor_1based)},
        {ElevatorReg::STATUS, 2, r8480},
        {ElevatorReg::CAR_CALLS, 2, 0},
        {ElevatorReg::HALL_CALLS_UP, 2, 0},
        {ElevatorReg::HALL_CALLS_DOWN, 2, 0},
        {ElevatorReg::CTB_INPUT, 2, r8471},
        {ElevatorReg::DOOR_OUTPUT, 2, r8472},
        {ElevatorReg::RUN_COUNTER_LOW, 2, static_cast<std::uint16_t>(run_cnt & 0xffffu)},
        {ElevatorReg::RUN_COUNTER_HIGH, 2, static_cast<std::uint16_t>(run_cnt >> 16)},
    };
    return u;
}

RegisterUpdate make_partial(std::uint64_t session, double t_ms,
                            std::vector<RegisterValueEntry> vals) {
    RegisterUpdate u;
    u.session     = session;
    u.received_ms = t_ms;
    u.addr_slave  = 2;
    u.values      = std::move(vals);
    return u;
}

} // namespace

int main() {
    // 1. Stopped -> Running -> Stopped, door timing (8471 vs 8472 fallback), idle silence (no gap),
    //    special mode flags, and 8448 mismatch detection.
    {
        ElevatorStateConfig cfg;
        cfg.run_timeout_ms = 60000.0;
        ElevatorStateTracker tracker(cfg);

        auto r0 = tracker.apply_update(make_full(1, 1000.0, 2, false, 16, 32, 100));
        CHECK(tracker.primary_timeline().state() == TimelineState::Stopped &&
                  r0.primary_events.size() == 2 &&
                  r0.primary_events[0].type == TimelineEventType::StopOpened &&
                  r0.primary_events[1].type == TimelineEventType::DoorOpened &&
                  r0.primary_events[0].stop.floor == 1,
              "initial snapshot opens stop and door via 8472 fallback");

        // Long idle silence (> 2 minutes) while Stopped must NOT trigger RegisterGap.
        auto p_idle = tracker.poll(150000.0, true, 1, false);
        CHECK(!p_idle.gap_triggered && tracker.primary_timeline().state() == TimelineState::Stopped,
              "long idle silence while Stopped does not trigger RegisterGap");

        // Door closes at 155000, then starts running at 156000 with maintenance bit set.
        auto r1 = tracker.apply_update(make_partial(
            1, 155000.0, {{ElevatorReg::CTB_INPUT, 2, 16}, {ElevatorReg::DOOR_OUTPUT, 2, 0}}));
        CHECK(r1.primary_events.size() == 1 &&
                  r1.primary_events[0].type == TimelineEventType::DoorClosed &&
                  r1.primary_events[0].stop.door_closed_at_ms == 155000.0,
              "records door close timestamp");

        auto r2 = tracker.apply_update(
            make_partial(1, 156000.0,
                         {{ElevatorReg::STATUS, 2,
                           static_cast<std::uint16_t>((1u << 2) | (1u << 0) | (1u << 3))}}));
        CHECK(tracker.primary_timeline().state() == TimelineState::Running &&
                  r2.primary_events.size() == 2 &&
                  r2.primary_events[0].type == TimelineEventType::StopClosed &&
                  r2.primary_events[1].type == TimelineEventType::RunStarted &&
                  (r2.primary_events[1].run.flags & TrafficFlag::MAINTENANCE) != 0,
              "Stopped->Running emits StopClosed and RunStarted with special flags");

        // Arrives at floor 6 (8465=7) with 8448 unchanged -> logs 1 run_counter_mismatch.
        auto r3 = tracker.apply_update(make_partial(1, 162000.0,
                                                    {{ElevatorReg::STATUS, 2, 1},
                                                     {ElevatorReg::FLOOR, 2, 7},
                                                     {ElevatorReg::CTB_INPUT, 2, 8}}));
        CHECK(tracker.primary_timeline().state() == TimelineState::Stopped &&
                  tracker.run_counter_mismatch_count() == 1 &&
                  r3.primary_events[0].type == TimelineEventType::RunEnded &&
                  r3.primary_events[0].run.from_floor == 1 &&
                  r3.primary_events[0].run.to_floor == 6 &&
                  r3.primary_events[1].type == TimelineEventType::StopOpened &&
                  r3.primary_events[2].type == TimelineEventType::DoorOpened,
              "Running->Stopped checks 8448 delta at RunEnded and opens destination stop");

        // Run timeout while Running triggers RegisterGap.
        tracker.apply_update(make_partial(
            1, 165000.0, {{ElevatorReg::STATUS, 2, static_cast<std::uint16_t>(1u << 2)}}));
        auto p_gap = tracker.poll(226000.0, true, 1, false);
        CHECK(p_gap.gap_triggered && tracker.primary_timeline().state() == TimelineState::Unknown &&
                  p_gap.primary_events.size() == 2 &&
                  p_gap.primary_events[0].type == TimelineEventType::RunEnded &&
                  (p_gap.primary_events[0].run.flags & TrafficFlag::REGISTER_GAP) != 0,
              "running timeout triggers RegisterGap and closes open run");
    }

    // 2. Session change & RegisterInbox overflow trigger RegisterGap
    {
        RegisterInbox inbox(2);
        inbox.push({"m1", 1, 10.0});
        inbox.push({"m2", 1, 20.0});
        inbox.push({"m3", 1, 30.0});
        std::vector<RawRegisterPacket> drained;
        bool                           overflowed = false;
        inbox.drain(drained, overflowed);
        CHECK(overflowed && drained.size() == 2 && drained[0].message == "m2" &&
                  drained[1].message == "m3",
              "RegisterInbox drops oldest on overflow and reports overflow flag");

        ElevatorStateTracker tracker(ElevatorStateConfig{});
        tracker.apply_update(make_full(1, 100.0, 2, false, 16, 32, 10));
        auto p_over = tracker.poll(200.0, true, 1, true);
        CHECK(p_over.gap_triggered && tracker.primary_timeline().state() == TimelineState::Unknown,
              "inbox overflow triggers RegisterGap");

        tracker.apply_update(make_full(2, 300.0, 2, false, 16, 32, 10));
        CHECK(tracker.primary_timeline().state() == TimelineState::Stopped,
              "full core registers restore valid state after gap");
        auto p_sess = tracker.poll(400.0, true, 3, false);
        CHECK(p_sess.gap_triggered && tracker.primary_timeline().state() == TimelineState::Unknown,
              "socket session increment triggers RegisterGap");
    }

    // 3. Replay real 10-minute fixture tests/fixtures/conn_data_20261103.jsonl
    {
        std::ifstream f("tests/fixtures/conn_data_20261103.jsonl");
        CHECK(f.is_open(), "opens real fixture tests/fixtures/conn_data_20261103.jsonl");
        ElevatorStateConfig  cfg;
        ElevatorStateTracker tracker(cfg);
        std::vector<int>     stop_floors;
        int                  closed_runs = 0;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            jm::JsonParser parser(line);
            jm::JsonValue  row;
            std::string    err;
            if (!parser.parse(row, err) || row.type != jm::JsonType::Object) continue;
            const auto* ev = jm::find(row, "event");
            if (!ev || ev->type != jm::JsonType::String || ev->string != "CONN_DATA") continue;
            const auto* t_val = jm::find(row, "t");
            const auto* body  = jm::find(row, "body");
            if (!t_val || t_val->type != jm::JsonType::Number || !body) continue;
            std::ostringstream body_ss;
            jm::serialize_value(body_ss, *body, 0);
            RegisterUpdate upd;
            if (!parse_conn_data_message(body_ss.str(), cfg.device_code, t_val->number * 1000.0, 1,
                                         upd))
                continue;
            const auto step = tracker.apply_update(upd);
            for (const auto& te : step.primary_events) {
                if (te.type == TimelineEventType::StopOpened) {
                    stop_floors.push_back(te.stop.floor);
                } else if (te.type == TimelineEventType::RunEnded) {
                    ++closed_runs;
                }
            }
        }

        const std::vector<int> expected_floors = {1, 5, 6, 7, 8, 3, 8, 1, 3, 5, 1, 6, 5};
        CHECK(closed_runs == 12, "fixture replay detects exactly 12 closed runs on slave 2");
        CHECK(tracker.run_counter_mismatch_count() == 0,
              "fixture replay matches 8448 run counter with zero mismatches");
        CHECK(stop_floors == expected_floors,
              "fixture replay matches expected 13 stop floors 1,5,6,7,8,3,8,1,3,5,1,6,5");
    }

    std::printf("\n[test_elevator_state] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
