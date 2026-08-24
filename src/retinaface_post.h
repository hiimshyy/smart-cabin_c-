#pragma once
// Postprocess: decode bbox + landmarks + softmax + NMS → detections
// trong toạ độ camera frame gốc.

#include <vector>
#include "anchors.h"
#include "retinaface_pre.h"

struct Detection {
    float x1, y1, x2, y2;    // bbox in original image coords
    float score;             // face confidence [0,1]
    float landmarks[10];     // (x0,y0,...,x4,y4) in original image coords
};

// Decode 3 output tensors (đã dequantize thành float32 bởi awnn_lib).
// Layout tensor: mỗi anchor lưu contiguous.
//   bbox_out:  4200 * 4 floats  → [dx, dy, dw, dh] per anchor
//   cls_out:   4200 * 2 floats  → [bg, face] logits per anchor (cần softmax)
//   lmk_out:   4200 * 10 floats → [dx0, dy0, ..., dx4, dy4] per anchor
void retinaface_postprocess(const float* bbox_out,
                            const float* cls_out,
                            const float* lmk_out,
                            const std::vector<Anchor>& anchors,
                            const PreInfo& pre,
                            float conf_thresh,
                            float nms_thresh,
                            std::vector<Detection>& out);
