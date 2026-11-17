#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <opencv2/imgproc.hpp>

namespace {

std::string format_floor_tag(int home_floor, int secondary_floor) {
    auto fmt = [](int floor) -> std::string {
        return floor == 0 ? std::string("B") : std::to_string(floor);
    };
    if (home_floor >= 0 && secondary_floor >= 0) {
        return "F" + fmt(home_floor) + "/" + fmt(secondary_floor);
    }
    if (home_floor >= 0) return "F" + fmt(home_floor);
    if (secondary_floor >= 0) return "F?/" + fmt(secondary_floor);
    return "F?";
}

} // namespace

// Moved verbatim from main.cpp (spec main-cpp-refactor). UiScale::compute is a
// pure numeric function; the two overlay branches and the HUD bar keep every
// cv:: call, color, thickness, font scale, and label snprintf format
// byte-for-byte identical to the Baseline.

UiScale UiScale::compute(int frame_h, float override_val) {
    UiScale u;
    // override_val > 0 forces that exact scale; 0 = auto by frame height
    u.scale      = (override_val > 0.0f) ? override_val : std::max(1.0f, frame_h / 480.0f);
    u.line_thick = std::max(1, (int)std::round(2.0f * u.scale));
    u.line_thin  = std::max(1, (int)std::round(1.0f * u.scale));
    u.text_thick = std::max(1, (int)std::round(1.5f * u.scale));
    u.text_thin  = std::max(1, (int)std::round(1.0f * u.scale));
    u.font_label = 0.6f * u.scale;
    u.font_hud   = 0.5f * u.scale;
    u.hud_h      = (int)std::round(32.0f * u.scale);
    u.hud_pad_x  = (int)std::round(6.0f * u.scale);
    u.hud_text_y = (int)std::round(20.0f * u.scale);
    u.landmark_r = std::max(1, (int)std::round(2.0f * u.scale));
    return u;
}

void draw_tracker_overlay(cv::Mat& frame, const std::vector<TrackView>& tracks,
                          const std::vector<Detection>&      faces,
                          const std::map<int64_t, Resident>& resident_by_id, const UiScale& ui,
                          bool use_resident_db) {
    for (const TrackView& t : tracks) {
        const bool known = t.identity.resident_id >= 0 && t.identity.latched;
        cv::Scalar color = t.state == ByteTrackState::Lost
                               ? cv::Scalar(140, 140, 140)  // stale predicted box
                           : known ? cv::Scalar(0, 255, 0)  // green
                                   : cv::Scalar(0, 0, 255); // red = unknown

        cv::rectangle(frame, cv::Point(cvRound(t.box.x1), cvRound(t.box.y1)),
                      cv::Point(cvRound(t.box.x2), cvRound(t.box.y2)), color, ui.line_thick);
        char lbl[128];
        if (known) {
            // In resident-db mode, append the home floor (or a marker
            // when unregistered) so the operator sees the destination.
            if (use_resident_db) {
                int  hf  = HOME_FLOOR_UNSET;
                int  sf  = SECONDARY_FLOOR_UNSET;
                auto rit = resident_by_id.find(t.identity.resident_id);
                if (rit != resident_by_id.end()) {
                    hf = rit->second.home_floor;
                    sf = rit->second.secondary_floor;
                }
                const std::string tag = format_floor_tag(hf, sf);
                std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f %s", t.id,
                              t.identity.display_name.c_str(), t.identity.similarity, tag.c_str());
            } else {
                std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f", t.id,
                              t.identity.display_name.c_str(), t.identity.similarity);
            }
        } else {
            std::snprintf(lbl, sizeof(lbl), "#%d unknown", t.id);
        }
        cv::putText(
            frame, lbl,
            cv::Point(cvRound(t.box.x1), cvRound(t.box.y1) - std::max(4, (int)(6 * ui.scale))),
            cv::FONT_HERSHEY_SIMPLEX, ui.font_label, color, ui.text_thick);
    }
    // Draw face bbox (small, thin) to visualize face detection quality
    for (const auto& fd : faces) {
        cv::rectangle(frame, cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                      cv::Point(cvRound(fd.x2), cvRound(fd.y2)), cv::Scalar(255, 255, 0),
                      ui.line_thin); // cyan
        for (int k = 0; k < 5; ++k) {
            cv::circle(frame,
                       cv::Point(cvRound(fd.landmarks[k * 2]), cvRound(fd.landmarks[k * 2 + 1])),
                       std::max(1, ui.landmark_r / 2), cv::Scalar(255, 255, 0), -1);
        }
    }
}

void draw_scrfd_overlay(cv::Mat& frame, const std::vector<Detection>& faces,
                        const std::vector<FaceLabel>&      face_labels,
                        const std::map<int64_t, Resident>& resident_by_id, const UiScale& ui,
                        bool recog_enabled, bool use_resident_db) {
    // SCRFD-only fallback: draw face bboxes with name
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto& fd         = faces[f];
        const auto& L          = face_labels[f];
        bool        is_unknown = recog_enabled && L.name == "unknown";
        cv::Scalar  color      = is_unknown ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
        cv::rectangle(frame, cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                      cv::Point(cvRound(fd.x2), cvRound(fd.y2)), color, ui.line_thick);
        char lbl[128];
        if (recog_enabled && !L.name.empty()) {
            // resident-db mode: append home floor for the matched person.
            if (use_resident_db && L.resident_id >= 0) {
                auto rit = resident_by_id.find(L.resident_id);
                int  hf = (rit != resident_by_id.end()) ? rit->second.home_floor : HOME_FLOOR_UNSET;
                int  sf = (rit != resident_by_id.end()) ? rit->second.secondary_floor
                                                        : SECONDARY_FLOOR_UNSET;
                const std::string tag = format_floor_tag(hf, sf);
                std::snprintf(lbl, sizeof(lbl), "%s %.2f %s", L.name.c_str(), L.sim, tag.c_str());
            } else {
                std::snprintf(lbl, sizeof(lbl), "%s %.2f", L.name.c_str(), L.sim);
            }
        } else {
            std::snprintf(lbl, sizeof(lbl), "%.0f%%", fd.score * 100.0f);
        }
        cv::putText(frame, lbl,
                    cv::Point(cvRound(fd.x1), cvRound(fd.y1) - std::max(4, (int)(6 * ui.scale))),
                    cv::FONT_HERSHEY_SIMPLEX, ui.font_label, color, ui.text_thick);
        static const cv::Scalar lm_colors[5] = {
            {0, 0, 255}, {0, 255, 255}, {255, 0, 255}, {0, 255, 0}, {255, 0, 0}};
        for (int k = 0; k < 5; ++k) {
            cv::circle(frame,
                       cv::Point(cvRound(fd.landmarks[k * 2]), cvRound(fd.landmarks[k * 2 + 1])),
                       ui.landmark_r, lm_colors[k], -1);
        }
    }
}

void draw_hud(cv::Mat& frame, const char* hud_text, const UiScale& ui) {
    {
        cv::Rect bg_rect(0, 0, frame.cols, ui.hud_h);
        cv::Mat  bg_roi = frame(bg_rect);
        cv::Mat  bg_layer(bg_roi.size(), bg_roi.type(), cv::Scalar(40, 40, 40));
        cv::addWeighted(bg_layer, 0.55, bg_roi, 0.45, 0, bg_roi);
    }
    cv::putText(frame, hud_text, cv::Point(ui.hud_pad_x, ui.hud_text_y), cv::FONT_HERSHEY_SIMPLEX,
                ui.font_hud, cv::Scalar(0, 255, 0), ui.text_thin);
}

void append_occupancy_hud(char* hud, std::size_t capacity, bool enabled, int count,
                          bool uncertain) {
    if (!enabled || !hud || capacity == 0) return;
    const std::size_t length = std::strlen(hud);
    if (length >= capacity) return;
    std::snprintf(hud + length, capacity - length, " | OCC %d%s", count, uncertain ? "?" : "");
}

void draw_counting_zones(cv::Mat& frame, const OccupancyConfig& config, int frame_w, int frame_h,
                         const UiScale& ui) {
    const cv::Scalar zone_color(0, 200, 255); // orange-ish
    const cv::Scalar arrow_color(0, 255, 0);  // green
    const float      arrow_len_px = 60.0f * ui.scale;

    for (const CountingZone& zone : config.zones) {
        std::vector<cv::Point> px = zone_to_pixels(zone, frame_w, frame_h);
        if (px.empty()) continue;

        // Translucent fill.
        cv::Mat                             overlay = frame.clone();
        std::vector<std::vector<cv::Point>> polys   = {px};
        cv::fillPoly(overlay, polys, cv::Scalar(0, 200, 255), cv::LINE_AA);
        cv::addWeighted(overlay, 0.20, frame, 0.80, 0, frame);

        // Boundary.
        cv::polylines(frame, polys, true, zone_color, std::max(1, ui.line_thin), cv::LINE_AA);

        // Direction arrow from centroid.
        cv::Point centroid(0, 0);
        for (const cv::Point& p : px)
            centroid += p;
        centroid.x /= static_cast<int>(px.size());
        centroid.y /= static_cast<int>(px.size());

        const float rad = zone.inside_direction_degrees * static_cast<float>(M_PI) / 180.0f;
        cv::Point   end(static_cast<int>(centroid.x + std::cos(rad) * arrow_len_px),
                        static_cast<int>(centroid.y + std::sin(rad) * arrow_len_px));
        cv::arrowedLine(frame, centroid, end, arrow_color, std::max(1, ui.line_thin), cv::LINE_AA,
                        0, 0.25);

        // Zone name near centroid.
        cv::putText(frame, zone.name.c_str(), cv::Point(centroid.x + 5, centroid.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, ui.font_label, zone_color, ui.text_thin);
    }
}
