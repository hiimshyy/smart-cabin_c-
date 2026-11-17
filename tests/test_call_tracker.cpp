#include "call_tracker.h"

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

RegisterUpdate make_snap(double t_ms, int floor_1based, bool running, std::uint16_t r8211,
                         std::uint16_t r8215, std::uint16_t r8225, std::uint16_t r8471,
                         std::uint16_t r8472) {
    RegisterUpdate u;
    u.session     = 1;
    u.received_ms = t_ms;
    u.addr_slave  = 2;
    u.values      = {
             {ElevatorReg::FLOOR, 2, static_cast<std::uint16_t>(floor_1based)},
             {ElevatorReg::STATUS, 2, static_cast<std::uint16_t>(running ? 4 : 1)},
             {ElevatorReg::CAR_CALLS, 2, r8211},
             {ElevatorReg::HALL_CALLS_UP, 2, r8215},
             {ElevatorReg::HALL_CALLS_DOWN, 2, r8225},
             {ElevatorReg::CTB_INPUT, 2, r8471},
             {ElevatorReg::DOOR_OUTPUT, 2, r8472},
    };
    return u;
}

RegisterUpdate make_reg(double t_ms, std::vector<RegisterValueEntry> vals) {
    RegisterUpdate u;
    u.session     = 1;
    u.received_ms = t_ms;
    u.addr_slave  = 2;
    u.values      = std::move(vals);
    return u;
}

} // namespace

int main() {
    // 1. Replay real 10-minute fixture tests/fixtures/conn_data_20261103.jsonl
    {
        std::ifstream f("tests/fixtures/conn_data_20261103.jsonl");
        CHECK(f.is_open(), "opens tests/fixtures/conn_data_20261103.jsonl");

        ElevatorStateTracker state_tracker(ElevatorStateConfig{});
        CallTracker          call_tracker(CallTrackerConfig{});

        int car_served          = 0;
        int hall_served         = 0;
        int hall_served_by_car1 = 0;
        int hall_other          = 0;
        int car_other           = 0;

        double      last_t_ms = 0.0;
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

            last_t_ms = t_val->number * 1000.0;
            std::ostringstream body_ss;
            jm::serialize_value(body_ss, *body, 0);
            RegisterUpdate upd;
            if (!parse_conn_data_message(body_ss.str(), "2026010900100098", last_t_ms, 1, upd))
                continue;

            const auto step = state_tracker.apply_update(upd);
            const auto res  = call_tracker.on_step(
                 state_tracker.primary_state(), state_tracker.primary_timeline().state(),
                 state_tracker.primary_timeline().recent_stops(),
                 state_tracker.secondary_timeline().recent_stops(), step.gap_triggered, last_t_ms);
            for (const auto& c : res.completed_car_calls) {
                if (c.outcome == "served")
                    ++car_served;
                else
                    ++car_other;
            }
            for (const auto& h : res.completed_hall_calls) {
                if (h.outcome == "served")
                    ++hall_served;
                else if (h.outcome == "served_by_car1")
                    ++hall_served_by_car1;
                else
                    ++hall_other;
            }
        }
        // Flush any remaining awaiting calls past tolerance
        const auto tail = call_tracker.poll(state_tracker.primary_timeline().recent_stops(),
                                            state_tracker.secondary_timeline().recent_stops(),
                                            last_t_ms + 20000.0);
        for (const auto& c : tail.completed_car_calls) {
            if (c.outcome == "served")
                ++car_served;
            else
                ++car_other;
        }
        for (const auto& h : tail.completed_hall_calls) {
            if (h.outcome == "served")
                ++hall_served;
            else if (h.outcome == "served_by_car1")
                ++hall_served_by_car1;
            else
                ++hall_other;
        }

        CHECK(car_served == 8 && car_other == 0,
              "fixture replay produces 8 served car calls on slave 2");
        CHECK(hall_served == 5 && hall_served_by_car1 == 6 && hall_other == 0,
              "fixture replay produces 5 hall served (car 2) and 6 served_by_car1");
    }

    // 2. Synthetic scenarios: registration_unobserved, auto floor-call confirmation/unconfirmed,
    //    registered_while_running, cancelled, cleared_without_stop, and register_gap.
    {
        ElevatorStateTracker est(ElevatorStateConfig{});
        CallTracker          ct(CallTrackerConfig{});

        // Initial snapshot already has hall_up bit 3 set -> registration_unobserved, wait_ms=-1
        auto s0 = est.apply_update(make_snap(1000.0, 2, false, 0, 1u << 3, 0, 16, 32));
        ct.on_step(est.primary_state(), est.primary_timeline().state(),
                   est.primary_timeline().recent_stops(), est.secondary_timeline().recent_stops(),
                   s0.gap_triggered, 1000.0);

        // Send auto floor call to floor 5; bit 5 activates 2s later -> source="auto" & confirmed
        ct.on_auto_call_sent("ai-core:2:1:1", 5, 2000.0);
        // Also send auto floor call to floor 7 that never gets registered -> register_unconfirmed
        ct.on_auto_call_sent("ai-core:2:1:2", 7, 2000.0);

        auto s1 = est.apply_update(make_reg(4000.0, {{ElevatorReg::CAR_CALLS, 2, 1u << 5}}));
        auto r1 = ct.on_step(est.primary_state(), est.primary_timeline().state(),
                             est.primary_timeline().recent_stops(),
                             est.secondary_timeline().recent_stops(), s1.gap_triggered, 4000.0);
        CHECK(r1.auto_confirmations.size() == 1 && r1.auto_confirmations[0].confirmed &&
                  r1.auto_confirmations[0].request_id == "ai-core:2:1:1",
              "8211 bit activation confirms matching SENT auto floor call");

        // Start running at t=5000, then press floor 6 mid-run at t=7000 -> registered_while_running
        auto s2 = est.apply_update(make_reg(5000.0, {{ElevatorReg::STATUS, 2, 4}}));
        ct.on_step(est.primary_state(), est.primary_timeline().state(),
                   est.primary_timeline().recent_stops(), est.secondary_timeline().recent_stops(),
                   s2.gap_triggered, 5000.0);
        auto s3 = est.apply_update(
            make_reg(7000.0, {{ElevatorReg::CAR_CALLS, 2, (1u << 5) | (1u << 6)}}));
        ct.on_step(est.primary_state(), est.primary_timeline().state(),
                   est.primary_timeline().recent_stops(), est.secondary_timeline().recent_stops(),
                   s3.gap_triggered, 7000.0);

        // Cancel floor 6 while still running; arrive at floor 5 and clear floor 5.
        auto s4 = est.apply_update(make_reg(8000.0, {{ElevatorReg::CAR_CALLS, 2, 1u << 5}}));
        ct.on_step(est.primary_state(), est.primary_timeline().state(),
                   est.primary_timeline().recent_stops(), est.secondary_timeline().recent_stops(),
                   s4.gap_triggered, 8000.0);

        // Arrive at floor 5 (8465=6) with door opening
        auto s5 = est.apply_update(make_reg(12000.0, {{ElevatorReg::STATUS, 2, 1},
                                                      {ElevatorReg::FLOOR, 2, 6},
                                                      {ElevatorReg::CTB_INPUT, 2, 8},
                                                      {ElevatorReg::CAR_CALLS, 2, 0}}));
        auto r5 = ct.on_step(est.primary_state(), est.primary_timeline().state(),
                             est.primary_timeline().recent_stops(),
                             est.secondary_timeline().recent_stops(), s5.gap_triggered, 12000.0);
        CHECK(r5.completed_car_calls.size() == 1 && r5.completed_car_calls[0].floor == 5 &&
                  r5.completed_car_calls[0].source == "auto" &&
                  r5.completed_car_calls[0].outcome == "served" &&
                  r5.completed_car_calls[0].origin_floor == 1,
              "auto car call closes as served with origin_floor and auto source");

        // Poll past floor 6 tolerance (8000 + 10001 = 18001) & floor 7 auto confirm (2000 + 15001)
        auto r6 = ct.poll(est.primary_timeline().recent_stops(),
                          est.secondary_timeline().recent_stops(), 18500.0);
        CHECK(r6.completed_car_calls.size() == 1 && r6.completed_car_calls[0].floor == 6 &&
                  r6.completed_car_calls[0].outcome == "cancelled" &&
                  (r6.completed_car_calls[0].flags & TrafficFlag::REGISTERED_WHILE_RUNNING) != 0 &&
                  r6.auto_confirmations.size() == 1 && !r6.auto_confirmations[0].confirmed &&
                  r6.auto_confirmations[0].request_id == "ai-core:2:1:2",
              "mid-run cancelled call carries registered_while_running and expired auto call is "
              "unconfirmed");

        // Disconnect triggers RegisterGap closing open hall_up call on floor 3 with
        // registration_unobserved
        auto p_gap = est.poll(19000.0, false, 1, false);
        // Note: was_connected_ initialized false; trigger session change instead
        auto p_gap2 = est.poll(19000.0, true, 2, false);
        auto rg =
            ct.on_step(est.primary_state(), est.primary_timeline().state(),
                       est.primary_timeline().recent_stops(),
                       est.secondary_timeline().recent_stops(), p_gap2.gap_triggered, 19000.0);
        CHECK(rg.completed_hall_calls.size() == 1 &&
                  rg.completed_hall_calls[0].outcome == "register_gap" &&
                  (rg.completed_hall_calls[0].flags & TrafficFlag::REGISTER_GAP) != 0 &&
                  (rg.completed_hall_calls[0].flags & TrafficFlag::REGISTRATION_UNOBSERVED) != 0 &&
                  rg.completed_hall_calls[0].wait_ms == -1,
              "register_gap closes unobserved open hall call without wait_ms");
        (void)p_gap;
    }

    std::printf("\n[test_call_tracker] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
