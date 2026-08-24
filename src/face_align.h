#pragma once
// Face alignment via 5-landmark affine transform.
// Canonical destination = ArcFace 112x112 template.

#include <opencv2/core.hpp>

// Standard ArcFace 5-point reference template for 112x112 aligned face.
// Order: left eye, right eye, nose, left mouth, right mouth.
extern const float kArcFaceRef112[5][2];

// Compute similarity transform (rotation + scale + translation, no shear)
// from `src_5pts` -> ArcFace canonical 112x112 template.
// Return 2x3 affine matrix (CV_32F). Uses Umeyama.
cv::Mat compute_align_transform(const float src_5pts[10]);

// One-shot: given full frame + 5 landmarks (image coords), returns aligned
// 112x112 BGR uint8 face crop.
//   frame     : BGR CV_8UC3
//   landmarks : 10 floats (x0,y0,...,x4,y4) in image coords
//   out       : output cv::Mat (allocated), CV_8UC3 112x112
void align_face_112(const cv::Mat& frame,
                    const float landmarks[10],
                    cv::Mat& out);
