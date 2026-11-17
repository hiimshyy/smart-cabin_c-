#include "elevator_registers.h"

#include "json_minimal.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <limits>
#include <string>

namespace jm = json_minimal;

namespace {

bool parse_uint_digits(const std::string& s, unsigned long long max_val, unsigned long long& out) {
    if (s.empty()) return false;
    unsigned long long acc = 0;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) return false;
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (acc > (max_val - digit) / 10ULL) return false;
        acc = acc * 10ULL + digit;
    }
    out = acc;
    return true;
}

bool parse_reg_slave_key(const std::string& key, int& reg, int& slave) {
    const size_t dash = key.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= key.size()) return false;
    if (key.find('-', dash + 1) != std::string::npos) return false;
    unsigned long long r = 0;
    unsigned long long s = 0;
    if (!parse_uint_digits(key.substr(0, dash), static_cast<unsigned long long>(INT_MAX), r) ||
        !parse_uint_digits(key.substr(dash + 1), static_cast<unsigned long long>(INT_MAX), s)) {
        return false;
    }
    reg   = static_cast<int>(r);
    slave = static_cast<int>(s);
    return true;
}

bool parse_uint16_json_value(const jm::JsonValue& val, std::uint16_t& out) {
    if (val.type == jm::JsonType::Number) {
        if (!std::isfinite(val.number) || std::floor(val.number) != val.number ||
            val.number < 0.0 || val.number > 65535.0) {
            return false;
        }
        out = static_cast<std::uint16_t>(val.number);
        return true;
    }
    if (val.type == jm::JsonType::String) {
        unsigned long long parsed = 0;
        if (!parse_uint_digits(val.string, 65535ULL, parsed)) return false;
        out = static_cast<std::uint16_t>(parsed);
        return true;
    }
    return false;
}

} // namespace

bool parse_conn_data_message(const std::string& message, const std::string& expected_device_code,
                             double received_ms, std::uint64_t session, RegisterUpdate& out) {
    out             = RegisterUpdate{};
    out.received_ms = received_ms;
    out.session     = session;

    std::string body_json;
    if (message.rfind("42[", 0) == 0) {
        jm::JsonParser parser(message.substr(2));
        jm::JsonValue  root;
        std::string    err;
        if (!parser.parse(root, err) || root.type != jm::JsonType::Array || root.array.size() < 2 ||
            root.array[0].type != jm::JsonType::String || root.array[0].string != "CONN_DATA" ||
            root.array[1].type != jm::JsonType::Object) {
            return false;
        }
        const jm::JsonValue& obj = root.array[1];
        const auto*          sn  = jm::find(obj, "sn");
        const auto*          lvl = jm::find(obj, "level");
        const auto*          dat = jm::find(obj, "data");
        if (!sn || !lvl || !dat || sn->type != jm::JsonType::String ||
            lvl->type != jm::JsonType::String || dat->type != jm::JsonType::Object) {
            return false;
        }
        if (!expected_device_code.empty() &&
            (sn->string != expected_device_code || lvl->string != expected_device_code)) {
            return false;
        }

        if (const auto* ts = jm::find(*dat, "TS"); ts && ts->type == jm::JsonType::String) {
            out.gateway_ts = ts->string;
        }
        if (const auto* mode = jm::find(*dat, "Mode"); mode && mode->type == jm::JsonType::String) {
            out.mode = mode->string;
        }
        int addr_val = -1;
        if (const auto* addr = jm::find(*dat, "ADDR"); addr && jm::as_int(*addr, addr_val)) {
            out.addr_slave = addr_val;
        }

        for (const auto& kv : dat->object) {
            int reg   = 0;
            int slave = 0;
            if (!parse_reg_slave_key(kv.first, reg, slave)) continue;
            std::uint16_t u16 = 0;
            if (!parse_uint16_json_value(kv.second, u16)) {
                ++out.invalid_values;
                continue;
            }
            out.values.push_back(RegisterValueEntry{reg, slave, u16});
        }
        return true;
    }

    // Raw JSON body form: {"sn":...,"level":...,"data":{...}}
    jm::JsonParser parser(message);
    jm::JsonValue  obj;
    std::string    err;
    if (!parser.parse(obj, err) || obj.type != jm::JsonType::Object) return false;
    const auto* sn  = jm::find(obj, "sn");
    const auto* lvl = jm::find(obj, "level");
    const auto* dat = jm::find(obj, "data");
    if (!sn || !lvl || !dat || sn->type != jm::JsonType::String ||
        lvl->type != jm::JsonType::String || dat->type != jm::JsonType::Object) {
        return false;
    }
    if (!expected_device_code.empty() &&
        (sn->string != expected_device_code || lvl->string != expected_device_code)) {
        return false;
    }

    if (const auto* ts = jm::find(*dat, "TS"); ts && ts->type == jm::JsonType::String) {
        out.gateway_ts = ts->string;
    }
    if (const auto* mode = jm::find(*dat, "Mode"); mode && mode->type == jm::JsonType::String) {
        out.mode = mode->string;
    }
    int addr_val = -1;
    if (const auto* addr = jm::find(*dat, "ADDR"); addr && jm::as_int(*addr, addr_val)) {
        out.addr_slave = addr_val;
    }

    for (const auto& kv : dat->object) {
        int reg   = 0;
        int slave = 0;
        if (!parse_reg_slave_key(kv.first, reg, slave)) continue;
        std::uint16_t u16 = 0;
        if (!parse_uint16_json_value(kv.second, u16)) {
            ++out.invalid_values;
            continue;
        }
        out.values.push_back(RegisterValueEntry{reg, slave, u16});
    }
    return true;
}

StatusRegister8480 decode_status_8480(std::uint16_t value) {
    StatusRegister8480 s;
    s.up          = (value & (1u << 0)) != 0;
    s.down        = (value & (1u << 1)) != 0;
    s.running     = (value & (1u << 2)) != 0;
    s.maintenance = (value & (1u << 3)) != 0;
    s.fault       = (value & (1u << 5)) != 0;
    s.parking     = (value & (1u << 6)) != 0;
    s.fire        = (value & (1u << 7)) != 0;
    s.independent = (value & (1u << 11)) != 0;
    return s;
}

DoorCtb8471 decode_door_ctb_8471(std::uint16_t value) {
    DoorCtb8471 d;
    d.overload     = (value & (1u << 2)) != 0;
    d.fully_open   = (value & (1u << 3)) != 0;
    d.fully_closed = (value & (1u << 4)) != 0;
    return d;
}

DoorOutput8472 decode_door_output_8472(std::uint16_t value) {
    DoorOutput8472 d;
    d.opening     = (value & (1u << 5)) != 0;
    d.closing     = (value & (1u << 6)) != 0;
    d.moving_up   = (value & (1u << 12)) != 0;
    d.moving_down = (value & (1u << 13)) != 0;
    return d;
}

bool decode_leveling_8467(std::uint16_t value) {
    return (value & (1u << 14)) != 0;
}

bool decode_floor_8465(std::uint16_t raw_value, int floor_min, int floor_max, int& floor_index) {
    if (raw_value == 0) return false;
    const int idx = static_cast<int>(raw_value) - 1;
    if (idx < floor_min || idx > floor_max) return false;
    floor_index = idx;
    return true;
}

std::uint16_t filter_floor_mask(std::uint16_t raw_mask, int floor_min, int floor_max,
                                int& invalid_bits_count) {
    std::uint16_t valid = 0;
    for (int bit = 0; bit < 16; ++bit) {
        if ((raw_mask & (1u << bit)) == 0) continue;
        if (bit < floor_min || bit > floor_max) {
            ++invalid_bits_count;
            continue;
        }
        valid = static_cast<std::uint16_t>(valid | (1u << bit));
    }
    return valid;
}

std::uint32_t combine_run_counter(std::uint16_t low_8448, std::uint16_t high_8449) {
    return (static_cast<std::uint32_t>(high_8449) << 16) | static_cast<std::uint32_t>(low_8448);
}

std::uint32_t status_to_traffic_flags(const StatusRegister8480& status) {
    std::uint32_t flags = 0;
    if (status.maintenance) flags |= TrafficFlag::MAINTENANCE;
    if (status.fault) flags |= TrafficFlag::FAULT;
    if (status.fire) flags |= TrafficFlag::FIRE;
    if (status.independent) flags |= TrafficFlag::INDEPENDENT;
    if (status.parking) flags |= TrafficFlag::PARKING;
    return flags;
}

std::string format_floor_label(int floor_index) {
    if (floor_index == 0) return "B";
    return std::to_string(floor_index);
}
