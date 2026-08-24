#pragma once
// Letterbox preprocessing: resize + pad giữ tỉ lệ, output HWC uint8 BGR
// đúng format model input (data_format=2 uint8, shape 320x320x3).

#include <cstdint>
#include <opencv2/core.hpp>

struct PreInfo {
    int   src_w, src_h;      // original camera frame size
    int   dst_w, dst_h;      // target NPU input size (320)
    int   pad_x, pad_y;      // padding offset in letterboxed image
    float scale;             // scale from src → letterboxed
};

// Letterbox resize `src` (BGR 8UC3) vào buffer `dst_hwc` size dst_w*dst_h*3 bytes.
// Trả về PreInfo để postprocess có thể invert map.
void retinaface_preprocess(const cv::Mat& src,
                           uint8_t* dst_hwc,
                           int dst_w, int dst_h,
                           PreInfo& info);
