#include "kalman_box_tracker.h"

#include <algorithm>
#include <cmath>

namespace {

bool is_finite(double value) { return std::isfinite(value); }

}  // namespace

KalmanBoxTracker::KalmanBoxTracker(const BoxF& initial_box, double timestamp_ms) {
    initialize(initial_box, timestamp_ms);
}

BoxF KalmanBoxTracker::sanitize(const BoxF& input) {
    BoxF out = input;
    if (!is_finite(out.x1)) out.x1 = 0.0f;
    if (!is_finite(out.y1)) out.y1 = 0.0f;
    if (!is_finite(out.x2)) out.x2 = out.x1 + 1.0f;
    if (!is_finite(out.y2)) out.y2 = out.y1 + 1.0f;
    if (out.x2 <= out.x1) out.x2 = out.x1 + 1.0f;
    if (out.y2 <= out.y1) out.y2 = out.y1 + 1.0f;
    return out;
}

void KalmanBoxTracker::initialize(const BoxF& initial_box, double timestamp_ms) {
    const BoxF box = sanitize(initial_box);
    const float width = box.x2 - box.x1;
    const float height = box.y2 - box.y1;
    const float cx = box.x1 + width * 0.5f;
    const float cy = box.y1 + height * 0.5f;

    filter_.init(8, 4, 0, CV_32F);
    filter_.transitionMatrix = cv::Mat::eye(8, 8, CV_32F);
    filter_.measurementMatrix = cv::Mat::zeros(4, 8, CV_32F);
    for (int i = 0; i < 4; ++i) filter_.measurementMatrix.at<float>(i, i) = 1.0f;
    cv::setIdentity(filter_.processNoiseCov, cv::Scalar::all(1e-2));
    cv::setIdentity(filter_.measurementNoiseCov, cv::Scalar::all(1.0));
    cv::setIdentity(filter_.errorCovPost, cv::Scalar::all(10.0));
    for (int i = 4; i < 8; ++i) filter_.errorCovPost.at<float>(i, i) = 1000.0f;
    filter_.statePost = cv::Mat::zeros(8, 1, CV_32F);
    filter_.statePost.at<float>(0) = cx;
    filter_.statePost.at<float>(1) = cy;
    filter_.statePost.at<float>(2) = width;
    filter_.statePost.at<float>(3) = height;
    filter_.statePre = filter_.statePost.clone();
    initialized_ = true;
    last_timestamp_ms_ = is_finite(timestamp_ms) ? timestamp_ms : 0.0;
}

void KalmanBoxTracker::set_transition(double dt_seconds) {
    filter_.transitionMatrix = cv::Mat::eye(8, 8, CV_32F);
    for (int i = 0; i < 4; ++i)
        filter_.transitionMatrix.at<float>(i, i + 4) = static_cast<float>(dt_seconds);

    const double q_pos = 0.05 + dt_seconds * dt_seconds;
    const double q_vel = 0.01 + dt_seconds;
    filter_.processNoiseCov = cv::Mat::zeros(8, 8, CV_32F);
    for (int i = 0; i < 4; ++i) {
        filter_.processNoiseCov.at<float>(i, i) = static_cast<float>(q_pos);
        filter_.processNoiseCov.at<float>(i + 4, i + 4) = static_cast<float>(q_vel);
    }
}

void KalmanBoxTracker::clamp_size(cv::Mat& state) const {
    state.at<float>(2) = std::max(1.0f, state.at<float>(2));
    state.at<float>(3) = std::max(1.0f, state.at<float>(3));
}

void KalmanBoxTracker::reset_velocity() {
    if (!initialized_) return;
    for (int i = 4; i < 8; ++i) {
        filter_.statePost.at<float>(i) = 0.0f;
        filter_.statePre.at<float>(i) = 0.0f;
    }
}

void KalmanBoxTracker::predict(double timestamp_ms, double max_predict_gap_ms) {
    if (!initialized_) return;
    double dt_ms = 0.0;
    if (is_finite(timestamp_ms) && is_finite(last_timestamp_ms_)) dt_ms = timestamp_ms - last_timestamp_ms_;
    if (!is_finite(dt_ms) || dt_ms <= 0.0) {
        dt_ms = 0.0;
    } else if (dt_ms > max_predict_gap_ms) {
        reset_velocity();
        dt_ms = 0.0;
    }
    set_transition(dt_ms / 1000.0);
    filter_.predict();
    clamp_size(filter_.statePre);
    // OpenCV keeps statePost at the last correction.  Promote this prediction
    // so repeated detector-skipped frames integrate velocity instead of always
    // extrapolating from the same measurement.
    filter_.statePost = filter_.statePre.clone();
    if (is_finite(timestamp_ms) && timestamp_ms >= last_timestamp_ms_)
        last_timestamp_ms_ = timestamp_ms;
}

void KalmanBoxTracker::correct(const BoxF& measurement, double timestamp_ms) {
    if (!initialized_) {
        initialize(measurement, timestamp_ms);
        return;
    }
    const BoxF box = sanitize(measurement);
    cv::Mat z(4, 1, CV_32F);
    z.at<float>(0) = (box.x1 + box.x2) * 0.5f;
    z.at<float>(1) = (box.y1 + box.y2) * 0.5f;
    z.at<float>(2) = box.x2 - box.x1;
    z.at<float>(3) = box.y2 - box.y1;
    filter_.correct(z);
    clamp_size(filter_.statePost);
    filter_.statePre = filter_.statePost.clone();
    if (is_finite(timestamp_ms)) last_timestamp_ms_ = timestamp_ms;
}

BoxF KalmanBoxTracker::box() const {
    if (!initialized_) return {};
    const cv::Mat& state = filter_.statePost;
    const float width = std::max(1.0f, state.at<float>(2));
    const float height = std::max(1.0f, state.at<float>(3));
    const float cx = state.at<float>(0);
    const float cy = state.at<float>(1);
    return {cx - width * 0.5f, cy - height * 0.5f,
            cx + width * 0.5f, cy + height * 0.5f};
}
