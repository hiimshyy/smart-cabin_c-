#include "occupancy_config.h"

#include "json_minimal.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace jm = json_minimal;

namespace {

std::atomic<unsigned long>          temp_sequence{0};
std::atomic<AtomicSaveFailurePoint> save_failure_point{AtomicSaveFailurePoint::None};

std::string parent_directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool write_all(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool atomic_write(const std::string& path, const std::string& data, std::string& error) {
    if (save_failure_point.load() == AtomicSaveFailurePoint::DirectoryOpen) {
        error = "cannot open config directory: injected failure";
        return false;
    }
    const int dir_fd = ::open(parent_directory(path).c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        error = "cannot open config directory: " + std::string(std::strerror(errno));
        return false;
    }

    const std::string temp = path + ".tmp." + std::to_string(static_cast<long long>(getpid())) +
                             "." + std::to_string(temp_sequence.fetch_add(1));
    if (save_failure_point.load() == AtomicSaveFailurePoint::TempOpen) {
        error = "cannot open temporary config: injected failure";
        ::close(dir_fd);
        return false;
    }
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        error = "cannot open temporary config: " + std::string(std::strerror(errno));
        ::close(dir_fd);
        return false;
    }
    bool ok = save_failure_point.load() != AtomicSaveFailurePoint::Write && write_all(fd, data);
    if (!ok) {
        error = save_failure_point.load() == AtomicSaveFailurePoint::Write
                    ? "cannot write temporary config: injected failure"
                    : "cannot write temporary config: " + std::string(std::strerror(errno));
    }
    if (ok && save_failure_point.load() == AtomicSaveFailurePoint::FileFsync) {
        ok    = false;
        error = "cannot fsync temporary config: injected failure";
    } else if (ok && fsync(fd) != 0) {
        ok    = false;
        error = "cannot fsync temporary config: " + std::string(std::strerror(errno));
    }
    if (::close(fd) != 0 && ok) {
        ok    = false;
        error = "cannot close temporary config: " + std::string(std::strerror(errno));
    }
    if (!ok) {
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }
    if (save_failure_point.load() == AtomicSaveFailurePoint::Rename) {
        error = "cannot rename temporary config: injected failure";
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "cannot rename temporary config: " + std::string(std::strerror(errno));
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }

    if (save_failure_point.load() == AtomicSaveFailurePoint::DirectoryFsync) {
        error = "cannot fsync config directory: injected failure";
        ::close(dir_fd);
        return false;
    }
    if (fsync(dir_fd) != 0) {
        error = "cannot fsync config directory: " + std::string(std::strerror(errno));
        ::close(dir_fd);
        return false;
    }
    ::close(dir_fd);
    return true;
}

} // namespace

void set_occupancy_config_save_failure_for_test(AtomicSaveFailurePoint point) {
    save_failure_point.store(point);
}

bool load_occupancy_config(const std::string& path, OccupancyConfig& out, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open occupancy config";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    jm::JsonParser parser(contents.str());
    jm::JsonValue  root;
    if (!parser.parse(root, error)) {
        error = "JSON parse error: " + error;
        return false;
    }
    if (root.type != jm::JsonType::Object) {
        error = "occupancy config root must be an object";
        return false;
    }

    OccupancyConfig      config;
    const jm::JsonValue* value = jm::find(root, "version");
    if (!value || !jm::as_int(*value, config.version)) {
        error = "missing/invalid 'version'";
        return false;
    }
    value = jm::find(root, "reference_width");
    if (!value || !jm::as_int(*value, config.reference_width)) {
        error = "missing/invalid 'reference_width'";
        return false;
    }
    value = jm::find(root, "reference_height");
    if (!value || !jm::as_int(*value, config.reference_height)) {
        error = "missing/invalid 'reference_height'";
        return false;
    }
    value = jm::find(root, "zones");
    if (!value || value->type != jm::JsonType::Array) {
        error = "missing/invalid 'zones'";
        return false;
    }

    for (const jm::JsonValue& zone_value : value->array) {
        if (zone_value.type != jm::JsonType::Object) {
            error = "zone must be an object";
            return false;
        }
        CountingZone         zone;
        const jm::JsonValue* name = jm::find(zone_value, "name");
        if (!name || !jm::as_string(*name, zone.name)) {
            error = "missing/invalid zone 'name'";
            return false;
        }
        const jm::JsonValue* polygon = jm::find(zone_value, "polygon");
        if (!polygon || polygon->type != jm::JsonType::Array) {
            error = "missing/invalid zone 'polygon'";
            return false;
        }
        for (const jm::JsonValue& point_value : polygon->array) {
            if (point_value.type != jm::JsonType::Object) {
                error = "invalid polygon point in zone '" + zone.name + "'";
                return false;
            }
            PointF               point;
            const jm::JsonValue* x = jm::find(point_value, "x");
            const jm::JsonValue* y = jm::find(point_value, "y");
            if (!x || !jm::as_float(*x, point.x) || !y || !jm::as_float(*y, point.y)) {
                error = "invalid polygon point in zone '" + zone.name + "'";
                return false;
            }
            zone.polygon.push_back(point);
        }
        const jm::JsonValue* direction = jm::find(zone_value, "inside_direction_degrees");
        if (!direction || !jm::as_float(*direction, zone.inside_direction_degrees)) {
            error = "missing/invalid zone 'inside_direction_degrees'";
            return false;
        }
        const jm::JsonValue* displacement = jm::find(zone_value, "min_crossing_displacement");
        if (!displacement || !jm::as_float(*displacement, zone.min_crossing_displacement)) {
            error = "missing/invalid zone 'min_crossing_displacement'";
            return false;
        }
        config.zones.push_back(std::move(zone));
    }

    if (!validate_occupancy_config(config, error)) return false;
    out = std::move(config);
    return true;
}

bool save_occupancy_config(const std::string& path, const OccupancyConfig& config,
                           std::string& error) {
    if (!validate_occupancy_config(config, error)) return false;

    jm::JsonValue root;
    root.type                              = jm::JsonType::Object;
    root.object["version"].type            = jm::JsonType::Number;
    root.object["version"].number          = config.version;
    root.object["reference_width"].type    = jm::JsonType::Number;
    root.object["reference_width"].number  = config.reference_width;
    root.object["reference_height"].type   = jm::JsonType::Number;
    root.object["reference_height"].number = config.reference_height;
    jm::JsonValue& zones                   = root.object["zones"];
    zones.type                             = jm::JsonType::Array;
    for (const CountingZone& zone : config.zones) {
        jm::JsonValue zone_value;
        zone_value.type                  = jm::JsonType::Object;
        zone_value.object["name"].type   = jm::JsonType::String;
        zone_value.object["name"].string = zone.name;
        jm::JsonValue& polygon           = zone_value.object["polygon"];
        polygon.type                     = jm::JsonType::Array;
        for (const PointF& point : zone.polygon) {
            jm::JsonValue point_value;
            point_value.type               = jm::JsonType::Object;
            point_value.object["x"].type   = jm::JsonType::Number;
            point_value.object["x"].number = point.x;
            point_value.object["y"].type   = jm::JsonType::Number;
            point_value.object["y"].number = point.y;
            polygon.array.push_back(std::move(point_value));
        }
        zone_value.object["inside_direction_degrees"].type    = jm::JsonType::Number;
        zone_value.object["inside_direction_degrees"].number  = zone.inside_direction_degrees;
        zone_value.object["min_crossing_displacement"].type   = jm::JsonType::Number;
        zone_value.object["min_crossing_displacement"].number = zone.min_crossing_displacement;
        zones.array.push_back(std::move(zone_value));
    }

    std::ostringstream output;
    jm::serialize_value(output, root, 0);
    output << '\n';
    return atomic_write(path, output.str(), error);
}

bool validate_occupancy_config(const OccupancyConfig& config, std::string& error) {
    if (config.version != OccupancyConfig::CURRENT_VERSION) {
        error = "unsupported config version " + std::to_string(config.version);
        return false;
    }
    if (config.reference_width <= 0 || config.reference_height <= 0) {
        error = "reference dimensions must be positive integers";
        return false;
    }
    if (config.zones.size() != 1) {
        error = "occupancy config v1 requires exactly one counting zone";
        return false;
    }

    const CountingZone& zone = config.zones.front();
    if (zone.name.empty()) {
        error = "zone name must not be empty";
        return false;
    }
    if (zone.polygon.size() < 3) {
        error = "zone '" + zone.name + "' polygon must have at least 3 points";
        return false;
    }
    double twice_area = 0.0;
    for (size_t i = 0; i < zone.polygon.size(); ++i) {
        const PointF& point = zone.polygon[i];
        const PointF& next  = zone.polygon[(i + 1) % zone.polygon.size()];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0f ||
            point.x > 1.0f || point.y < 0.0f || point.y > 1.0f) {
            error = "zone '" + zone.name + "' has point outside [0,1]";
            return false;
        }
        twice_area += static_cast<double>(point.x) * next.y - static_cast<double>(next.x) * point.y;
    }
    if (std::abs(twice_area) < 1e-9) {
        error = "zone '" + zone.name + "' polygon must have non-zero area";
        return false;
    }
    if (!std::isfinite(zone.inside_direction_degrees) || zone.inside_direction_degrees < 0.0f ||
        zone.inside_direction_degrees >= 360.0f) {
        error = "zone '" + zone.name + "' inside_direction_degrees out of range";
        return false;
    }
    if (!std::isfinite(zone.min_crossing_displacement) || zone.min_crossing_displacement < 0.0f ||
        zone.min_crossing_displacement > 1.0f) {
        error = "zone '" + zone.name + "' min_crossing_displacement out of range";
        return false;
    }
    return true;
}

std::vector<cv::Point> zone_to_pixels(const CountingZone& zone, int frame_w, int frame_h) {
    std::vector<cv::Point> pixels;
    pixels.reserve(zone.polygon.size());
    for (const PointF& point : zone.polygon) {
        pixels.emplace_back(static_cast<int>(std::round(point.x * static_cast<float>(frame_w))),
                            static_cast<int>(std::round(point.y * static_cast<float>(frame_h))));
    }
    return pixels;
}
