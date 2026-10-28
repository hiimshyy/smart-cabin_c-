#pragma once

#include "atomic_save_test.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct CountingZone {
    std::string         name;
    std::vector<PointF> polygon;                          // normalized [0, 1]
    float               inside_direction_degrees  = 0.0f; // vector pointing into the cabin
    float               min_crossing_displacement = 0.0f; // normalized; abort if not met
};

struct OccupancyConfig {
    static constexpr int CURRENT_VERSION = 1;

    int                       version          = CURRENT_VERSION;
    int                       reference_width  = 640;
    int                       reference_height = 480;
    std::vector<CountingZone> zones;
};

bool load_occupancy_config(const std::string& path, OccupancyConfig& out, std::string& error);
bool save_occupancy_config(const std::string& path, const OccupancyConfig& cfg, std::string& error);
bool validate_occupancy_config(const OccupancyConfig& cfg, std::string& error);
void set_occupancy_config_save_failure_for_test(AtomicSaveFailurePoint point);

// Convert a normalized zone polygon to pixel coordinates for the current frame size.
std::vector<cv::Point> zone_to_pixels(const CountingZone& zone, int frame_w, int frame_h);
