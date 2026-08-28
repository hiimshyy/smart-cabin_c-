#pragma once
// Letterbox preprocessing for face detector NPU input.
// Resize + pad keep aspect ratio, output HWC uint8 BGR contiguous.
// Compatible with SCRFD 640×640 uint8 input models.

#include <cstdint>
#include <opencv2/core.hpp>

struct PreInfo {
    int   src_w, src_h;      // original camera frame size
    int   dst_w, dst_h;      // target NPU input size
    int   pad_x, pad_y;      // padding offset in letterboxed image
    float scale;             // scale from src → letterboxed
};

// Letterbox resize `src` (BGR 8UC3) into `dst_hwc` buffer of size dst_w*dst_h*3.
// Fills PreInfo so downstream postprocess can invert the letterbox mapping.
void detect_preprocess(const cv::Mat& src,
                       uint8_t* dst_hwc,
                       int dst_w, int dst_h,
                       PreInfo& info);
