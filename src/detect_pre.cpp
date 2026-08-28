#include "detect_pre.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>

void detect_preprocess(const cv::Mat& src,
                       uint8_t* dst_hwc,
                       int dst_w, int dst_h,
                       PreInfo& info) {
    info.src_w = src.cols;
    info.src_h = src.rows;
    info.dst_w = dst_w;
    info.dst_h = dst_h;

    // Keep aspect ratio: shrink so the longer side fits, pad the rest.
    float scale = std::min(static_cast<float>(dst_w) / info.src_w,
                           static_cast<float>(dst_h) / info.src_h);
    int   new_w = static_cast<int>(info.src_w * scale);
    int   new_h = static_cast<int>(info.src_h * scale);
    int   pad_x = (dst_w - new_w) / 2;
    int   pad_y = (dst_h - new_h) / 2;
    info.scale  = scale;
    info.pad_x  = pad_x;
    info.pad_y  = pad_y;

    // Wrap output buffer as an HWC BGR uint8 image, then resize the source
    // into the centered ROI. cv::Mat 8UC3 is HWC (BGR-BGR-BGR interleaved).
    cv::Mat canvas(dst_h, dst_w, CV_8UC3, dst_hwc);
    canvas.setTo(cv::Scalar(0, 0, 0));

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
    cv::Mat roi(canvas, cv::Rect(pad_x, pad_y, new_w, new_h));
    resized.copyTo(roi);
}
