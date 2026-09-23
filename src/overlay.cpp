#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <opencv2/imgproc.hpp>

// Moved verbatim from main.cpp (spec main-cpp-refactor). UiScale::compute is a
// pure numeric function; the two overlay branches and the HUD bar keep every
// cv:: call, color, thickness, font scale, and label snprintf format
// byte-for-byte identical to the Baseline.

UiScale UiScale::compute(int frame_h, float override_val) {
    UiScale u;
    // override_val > 0 forces that exact scale; 0 = auto by frame height
    u.scale = (override_val > 0.0f) ? override_val
                                    : std::max(1.0f, frame_h / 480.0f);
    u.line_thick = std::max(1, (int)std::round(2.0f  * u.scale));
    u.line_thin  = std::max(1, (int)std::round(1.0f  * u.scale));
    u.text_thick = std::max(1, (int)std::round(1.5f  * u.scale));
    u.text_thin  = std::max(1, (int)std::round(1.0f  * u.scale));
    u.font_label = 0.6f * u.scale;
    u.font_hud   = 0.5f * u.scale;
    u.hud_h      = (int)std::round(32.0f * u.scale);
    u.hud_pad_x  = (int)std::round(6.0f  * u.scale);
    u.hud_text_y = (int)std::round(20.0f * u.scale);
    u.landmark_r = std::max(1, (int)std::round(2.0f  * u.scale));
    return u;
}

void draw_tracker_overlay(cv::Mat& frame, Tracker& tracker,
                          const std::vector<Detection>& faces,
                          const std::map<int, int64_t>& track_resident_id,
                          const std::map<int64_t, Resident>& resident_by_id,
                          const UiScale& ui, bool use_resident_db) {
    for (Track* t : tracker.active_tracks()) {
        bool known    = !t->name.empty() && t->name != "unknown";
        bool unknown_tried = t->name == "unknown";
        cv::Scalar color = known ? cv::Scalar(0, 255, 0)     // green
                         : unknown_tried ? cv::Scalar(0, 0, 255)   // red
                         : cv::Scalar(200, 200, 0);          // yellow (untried)

        cv::rectangle(frame,
                      cv::Point(cvRound(t->x1), cvRound(t->y1)),
                      cv::Point(cvRound(t->x2), cvRound(t->y2)),
                      color, ui.line_thick);
        char lbl[128];
        if (known) {
            // In resident-db mode, append the home floor (or a marker
            // when unregistered) so the operator sees the destination.
            if (use_resident_db) {
                int hf = -999;
                auto nit = track_resident_id.find(t->id);
                if (nit != track_resident_id.end()) {
                    auto rit = resident_by_id.find(nit->second);
                    if (rit != resident_by_id.end()) hf = rit->second.home_floor;
                }
                if (hf > HOME_FLOOR_UNSET) {
                    std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f F%d",
                                  t->id, t->name.c_str(), t->match_sim, hf);
                } else {
                    std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f F?",
                                  t->id, t->name.c_str(), t->match_sim);
                }
            } else {
                std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f",
                              t->id, t->name.c_str(), t->match_sim);
            }
        } else if (unknown_tried) {
            std::snprintf(lbl, sizeof(lbl), "#%d unknown", t->id);
        } else {
            std::snprintf(lbl, sizeof(lbl), "#%d ...", t->id);
        }
        cv::putText(frame, lbl,
                    cv::Point(cvRound(t->x1),
                              cvRound(t->y1) - std::max(4, (int)(6 * ui.scale))),
                    cv::FONT_HERSHEY_SIMPLEX, ui.font_label,
                    color, ui.text_thick);
    }
    // Draw face bbox (small, thin) to visualize face detection quality
    for (const auto& fd : faces) {
        cv::rectangle(frame,
                      cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                      cv::Point(cvRound(fd.x2), cvRound(fd.y2)),
                      cv::Scalar(255, 255, 0), ui.line_thin);   // cyan
        for (int k = 0; k < 5; ++k) {
            cv::circle(frame,
                       cv::Point(cvRound(fd.landmarks[k*2]),
                                 cvRound(fd.landmarks[k*2+1])),
                       std::max(1, ui.landmark_r / 2),
                       cv::Scalar(255, 255, 0), -1);
        }
    }
}

void draw_scrfd_overlay(cv::Mat& frame,
                        const std::vector<Detection>& faces,
                        const std::vector<FaceLabel>& face_labels,
                        const std::map<int64_t, Resident>& resident_by_id,
                        const UiScale& ui, bool recog_enabled,
                        bool use_resident_db) {
    // SCRFD-only fallback: draw face bboxes with name
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto& fd = faces[f];
        const auto& L  = face_labels[f];
        bool is_unknown = recog_enabled && L.name == "unknown";
        cv::Scalar color = is_unknown
            ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
        cv::rectangle(frame,
                      cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                      cv::Point(cvRound(fd.x2), cvRound(fd.y2)),
                      color, ui.line_thick);
        char lbl[128];
        if (recog_enabled && !L.name.empty()) {
            // resident-db mode: append home floor for the matched person.
            if (use_resident_db && L.resident_id >= 0) {
                auto rit = resident_by_id.find(L.resident_id);
                int hf = (rit != resident_by_id.end()) ? rit->second.home_floor : 0;
                if (hf > HOME_FLOOR_UNSET) {
                    std::snprintf(lbl, sizeof(lbl), "%s %.2f F%d",
                                  L.name.c_str(), L.sim, hf);
                } else {
                    std::snprintf(lbl, sizeof(lbl), "%s %.2f F?",
                                  L.name.c_str(), L.sim);
                }
            } else {
                std::snprintf(lbl, sizeof(lbl), "%s %.2f", L.name.c_str(), L.sim);
            }
        } else {
            std::snprintf(lbl, sizeof(lbl), "%.0f%%", fd.score * 100.0f);
        }
        cv::putText(frame, lbl,
                    cv::Point(cvRound(fd.x1),
                              cvRound(fd.y1) - std::max(4, (int)(6 * ui.scale))),
                    cv::FONT_HERSHEY_SIMPLEX, ui.font_label,
                    color, ui.text_thick);
        static const cv::Scalar lm_colors[5] = {
            {0,0,255}, {0,255,255}, {255,0,255}, {0,255,0}, {255,0,0}
        };
        for (int k = 0; k < 5; ++k) {
            cv::circle(frame,
                       cv::Point(cvRound(fd.landmarks[k*2]),
                                 cvRound(fd.landmarks[k*2+1])),
                       ui.landmark_r, lm_colors[k], -1);
        }
    }
}

void draw_hud(cv::Mat& frame, const char* hud_text, const UiScale& ui) {
    {
        cv::Rect bg_rect(0, 0, frame.cols, ui.hud_h);
        cv::Mat  bg_roi   = frame(bg_rect);
        cv::Mat  bg_layer(bg_roi.size(), bg_roi.type(),
                          cv::Scalar(40, 40, 40));
        cv::addWeighted(bg_layer, 0.55, bg_roi, 0.45, 0, bg_roi);
    }
    cv::putText(frame, hud_text, cv::Point(ui.hud_pad_x, ui.hud_text_y),
                cv::FONT_HERSHEY_SIMPLEX, ui.font_hud,
                cv::Scalar(0, 255, 0), ui.text_thin);
}
