#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TrafficFlag {
constexpr std::uint32_t OCCUPANCY_UNCERTAIN_IN   = 1u << 0;
constexpr std::uint32_t OCCUPANCY_UNCERTAIN_OUT  = 1u << 1;
constexpr std::uint32_t ENTRY_UNOBSERVED         = 1u << 2;
constexpr std::uint32_t EXIT_UNOBSERVED          = 1u << 3;
constexpr std::uint32_t FLOOR_UNKNOWN            = 1u << 4;
constexpr std::uint32_t REGISTER_GAP             = 1u << 5;
constexpr std::uint32_t NO_RIDE                  = 1u << 6;
constexpr std::uint32_t IDENTITY_CONFLICT        = 1u << 7;
constexpr std::uint32_t SHUTDOWN                 = 1u << 8;
constexpr std::uint32_t MAINTENANCE              = 1u << 9;
constexpr std::uint32_t FAULT                    = 1u << 10;
constexpr std::uint32_t FIRE                     = 1u << 11;
constexpr std::uint32_t INDEPENDENT              = 1u << 12;
constexpr std::uint32_t PARKING                  = 1u << 13;
constexpr std::uint32_t OVERLOAD                 = 1u << 14;
constexpr std::uint32_t LOAD_IMPLAUSIBLE         = 1u << 15;
constexpr std::uint32_t REGISTERED_WHILE_RUNNING = 1u << 16;
constexpr std::uint32_t REGISTRATION_UNOBSERVED  = 1u << 17;
} // namespace TrafficFlag

namespace ElevatorReg {
constexpr int RUN_COUNTER_LOW  = 8448;
constexpr int RUN_COUNTER_HIGH = 8449;
constexpr int HEIGHT_CM        = 8464;
constexpr int FLOOR            = 8465;
constexpr int LEVELING         = 8467;
constexpr int CTB_INPUT        = 8471;
constexpr int DOOR_OUTPUT      = 8472;
constexpr int STATUS           = 8480;
constexpr int CAR_CALLS        = 8211;
constexpr int HALL_CALLS_UP    = 8215;
constexpr int HALL_CALLS_DOWN  = 8225;
} // namespace ElevatorReg

struct RegisterValueEntry {
    int           reg   = 0;
    int           slave = 0;
    std::uint16_t value = 0;
};

struct RegisterUpdate {
    std::uint64_t                   session     = 0;
    double                          received_ms = 0.0;
    std::string                     gateway_ts;
    std::string                     mode;
    int                             addr_slave     = -1;
    int                             invalid_values = 0;
    std::vector<RegisterValueEntry> values;
};

struct StatusRegister8480 {
    bool up          = false;
    bool down        = false;
    bool running     = false;
    bool maintenance = false;
    bool fault       = false;
    bool parking     = false;
    bool fire        = false;
    bool independent = false;
};

struct DoorCtb8471 {
    bool overload     = false;
    bool fully_open   = false;
    bool fully_closed = false;
};

struct DoorOutput8472 {
    bool opening     = false;
    bool closing     = false;
    bool moving_up   = false;
    bool moving_down = false;
};

// Parse either a Socket.IO `42["CONN_DATA",{...}]` frame or its JSON body object `{...}`.
// Rejects messages whose sn/level fail to match expected_device_code (when non-empty).
// Non-16-bit or malformed values on `<reg>-<slave>` entries are skipped and counted in
// `out.invalid_values` without rejecting valid entries in the same message.
bool parse_conn_data_message(const std::string& message, const std::string& expected_device_code,
                             double received_ms, std::uint64_t session, RegisterUpdate& out);

StatusRegister8480 decode_status_8480(std::uint16_t value);
DoorCtb8471        decode_door_ctb_8471(std::uint16_t value);
DoorOutput8472     decode_door_output_8472(std::uint16_t value);
bool               decode_leveling_8467(std::uint16_t value);

// Convert INVT 8465 (1-based, 1 = B) to 0-based floor_index in [floor_min, floor_max].
bool decode_floor_8465(std::uint16_t raw_value, int floor_min, int floor_max, int& floor_index);

// Retain only bits n in [floor_min, floor_max]. Any set bit outside that range increments
// `invalid_bits_count` and is cleared from the returned mask.
std::uint16_t filter_floor_mask(std::uint16_t raw_mask, int floor_min, int floor_max,
                                int& invalid_bits_count);

std::uint32_t combine_run_counter(std::uint16_t low_8448, std::uint16_t high_8449);
std::uint32_t status_to_traffic_flags(const StatusRegister8480& status);
std::string   format_floor_label(int floor_index);
