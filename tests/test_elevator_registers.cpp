#include "elevator_registers.h"

#include <cstdio>

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
    RegisterUpdate u;

    // 1. Socket.IO frame with multiple slaves and partial update
    CHECK(
        parse_conn_data_message(
            R"(42["CONN_DATA",{"sn":"2026010900100098","level":"2026010900100098","data":{"ADDR":2,"Mode":"new","TS":"2026-11-03T07:49:34.292Z","8465-2":2,"8480-2":"1285","8472-1":32,"8211-2":96}}])",
            "2026010900100098", 1234.5, 7, u),
        "parses valid Socket.IO CONN_DATA");
    CHECK(u.session == 7 && u.received_ms == 1234.5 && u.addr_slave == 2 && u.mode == "new" &&
              u.gateway_ts == "2026-11-03T07:49:34.292Z" && u.invalid_values == 0 &&
              u.values.size() == 4,
          "extracts metadata, numeric strings, and multi-slave entries");

    // 2. Raw JSON body form
    CHECK(
        parse_conn_data_message(
            R"({"sn":"2026010900100098","level":"2026010900100098","data":{"8465-2":1,"8464-2":314}})",
            "2026010900100098", 2000.0, 1, u) &&
            u.values.size() == 2,
        "parses raw JSON body form");

    // 3. Rejects wrong device code or non-CONN_DATA event
    CHECK(!parse_conn_data_message(
              R"(42["CONN_DATA",{"sn":"wrong","level":"2026010900100098","data":{"8465-2":2}}])",
              "2026010900100098", 0.0, 1, u),
          "rejects mismatched sn");
    CHECK(
        !parse_conn_data_message(
            R"(42["CONN_SUBSCRIBE_RESULT",{"sn":"2026010900100098","level":"2026010900100098","data":{"8465-2":2}}])",
            "2026010900100098", 0.0, 1, u),
        "rejects non-CONN_DATA event");

    // 4. Rejects non-16-bit values while keeping valid entries
    CHECK(
        parse_conn_data_message(
            R"(42["CONN_DATA",{"sn":"2026010900100098","level":"2026010900100098","data":{"8465-2":70000,"8480-2":-1,"8471-2":12.5,"8472-2":"bad","8211-2":32}}])",
            "2026010900100098", 0.0, 1, u),
        "tolerates partial invalid values");
    CHECK(u.invalid_values == 4 && u.values.size() == 1 && u.values[0].reg == 8211 &&
              u.values[0].value == 32,
          "rejects out-of-range/float/non-numeric register values and keeps valid ones");

    // 5. Floor decoding and bitmask filtering
    int fl = -1;
    CHECK(decode_floor_8465(1, 0, 9, fl) && fl == 0 && format_floor_label(fl) == "B",
          "8465=1 maps to floor 0 (B)");
    CHECK(decode_floor_8465(10, 0, 9, fl) && fl == 9 && format_floor_label(fl) == "9",
          "8465=10 maps to floor 9");
    CHECK(!decode_floor_8465(0, 0, 9, fl), "8465=0 rejected");
    CHECK(!decode_floor_8465(11, 0, 9, fl), "8465=11 outside [0,9] rejected");

    int           invalid_bits = 0;
    std::uint16_t masked       = filter_floor_mask(
              static_cast<std::uint16_t>((1u << 0) | (1u << 5) | (1u << 10)), 0, 9, invalid_bits);
    CHECK(masked == ((1u << 0) | (1u << 5)) && invalid_bits == 1,
          "filter_floor_mask strips bits outside [floor_min, floor_max]");

    // 6. Status / door / leveling / run counter bit decoders
    const auto st = decode_status_8480(1285); // bits 0, 2, 8, 10
    CHECK(st.up && !st.down && st.running && !st.maintenance,
          "decodes 8480 up/running bits from 1285");
    const auto st_special =
        decode_status_8480((1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 11));
    CHECK(status_to_traffic_flags(st_special) ==
              (TrafficFlag::MAINTENANCE | TrafficFlag::FAULT | TrafficFlag::PARKING |
               TrafficFlag::FIRE | TrafficFlag::INDEPENDENT),
          "maps special status bits to traffic flags");
    const auto ctb = decode_door_ctb_8471((1u << 2) | (1u << 3));
    CHECK(ctb.overload && ctb.fully_open && !ctb.fully_closed, "decodes 8471 CTB bits");
    const auto door = decode_door_output_8472(4175); // includes bit 5/6/12 etc.
    CHECK(door.moving_up && !door.moving_down, "decodes 8472 moving_up bit");
    CHECK(decode_leveling_8467(49408) && !decode_leveling_8467(33072),
          "decodes 8467 bit 14 leveling");
    CHECK(combine_run_counter(45590, 1) == 111126u, "combines 8448/8449 32-bit counter");

    std::printf("\n[test_elevator_registers] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
