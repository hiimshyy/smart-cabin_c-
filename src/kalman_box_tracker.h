#pragma once

#include "tracker_types.h"

#include <opencv2/video/tracking.hpp>

// Constant-velocity Kalman filter for a person bounding box.  Coordinates are
// represented internally as cx/cy/w/h and velocities use pixels per second.
class KalmanBoxTracker {
public:
    KalmanBoxTracker() = default;
    KalmanBoxTracker(const BoxF& initial_box, double timestamp_ms);

    void initialize(const BoxF& initial_box, double timestamp_ms);
    void predict(double timestamp_ms, double max_predict_gap_ms);
    void correct(const BoxF& measurement, double timestamp_ms);
    void reset_velocity();

    BoxF box() const;
    bool initialized() const { return initialized_; }
    double last_timestamp_ms() const { return last_timestamp_ms_; }

private:
    static BoxF sanitize(const BoxF& box);
    void set_transition(double dt_seconds);
    void clamp_size(cv::Mat& state) const;

    cv::KalmanFilter filter_{8, 4, 0, CV_32F};
    bool initialized_ = false;
    double last_timestamp_ms_ = -1.0;
};
