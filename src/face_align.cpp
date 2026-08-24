#include "face_align.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <vector>

// Canonical 5-point ArcFace reference for 112x112 aligned face.
// Source: InsightFace / ArcFace repo (used by all common ArcFace variants).
const float kArcFaceRef112[5][2] = {
    {38.2946f, 51.6963f},    // left eye
    {73.5318f, 51.5014f},    // right eye
    {56.0252f, 71.7366f},    // nose tip
    {41.5493f, 92.3655f},    // left mouth corner
    {70.7299f, 92.2041f},    // right mouth corner
};

cv::Mat compute_align_transform(const float src_5pts[10]) {
    std::vector<cv::Point2f> src(5), dst(5);
    for (int i = 0; i < 5; ++i) {
        src[i] = cv::Point2f(src_5pts[i * 2 + 0], src_5pts[i * 2 + 1]);
        dst[i] = cv::Point2f(kArcFaceRef112[i][0], kArcFaceRef112[i][1]);
    }
    // Similarity transform: rotation + uniform scale + translation (no shear).
    // Use LMEDS/no-RANSAC since we have 5 clean points.
    cv::Mat M = cv::estimateAffinePartial2D(src, dst,
                                            cv::noArray(),
                                            cv::LMEDS);
    if (M.empty()) {
        // Fallback: identity, no crop — degenerate landmarks
        M = cv::Mat::eye(2, 3, CV_64F);
    }
    // OpenCV returns CV_64F; warpAffine accepts both.
    return M;
}

void align_face_112(const cv::Mat& frame,
                    const float landmarks[10],
                    cv::Mat& out) {
    cv::Mat M = compute_align_transform(landmarks);
    cv::warpAffine(frame, out, M, cv::Size(112, 112),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
}
