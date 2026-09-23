#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include <opencv2/core.hpp>

#include "detection.h"     // Detection
#include "tracker.h"       // Track, Tracker
#include "resident_db.h"   // Resident
#include "face_label.h"    // FaceLabel

// UI scaling + overlay/HUD drawing (spec main-cpp-refactor, Overlay_Module).
// UiScale::compute and all draw code are moved verbatim; colors, thicknesses,
// font scales, and label snprintf formats are byte-for-byte identical.

// Overlay sizing helper — keeps text/box visually proportional to frame.
// Baseline is 480p; scale linearly with frame height so text looks the
// same relative size on 720p / 1080p / etc.
struct UiScale {
    float scale       = 1.0f;   // multiplier vs baseline 480p
    int   line_thick  = 2;      // rectangle line thickness (person track)
    int   line_thin   = 1;      // rectangle line thickness (face bbox)
    int   text_thick  = 2;      // putText thickness (labels)
    int   text_thin   = 1;      // putText thickness (HUD)
    float font_label  = 0.6f;   // putText fontScale (labels)
    float font_hud    = 0.5f;   // putText fontScale (HUD)
    int   hud_h       = 32;     // HUD background rectangle height (px)
    int   hud_pad_x   = 6;
    int   hud_text_y  = 20;
    int   landmark_r  = 2;      // circle radius for face landmarks

    static UiScale compute(int frame_h, float override_val);
};

// Tracker-mode overlay: person track rectangles + labels, then face bboxes.
void draw_tracker_overlay(cv::Mat& frame, Tracker& tracker,
                          const std::vector<Detection>& faces,
                          const std::map<int, int64_t>& track_resident_id,
                          const std::map<int64_t, Resident>& resident_by_id,
                          const UiScale& ui, bool use_resident_db);

// SCRFD-only overlay: face bbox + label + landmarks.
void draw_scrfd_overlay(cv::Mat& frame,
                        const std::vector<Detection>& faces,
                        const std::vector<FaceLabel>& face_labels,
                        const std::map<int64_t, Resident>& resident_by_id,
                        const UiScale& ui, bool recog_enabled,
                        bool use_resident_db);

// HUD bar: darkened top strip + timing string.
void draw_hud(cv::Mat& frame, const char* hud_text, const UiScale& ui);
