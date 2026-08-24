#include "retinaface_post.h"
#include <algorithm>
#include <cmath>

namespace {

// Softmax 2-class → return probability of class 1 (face).
inline float softmax_face_prob(float bg_logit, float face_logit) {
    float m = std::max(bg_logit, face_logit);
    float e_bg   = std::exp(bg_logit   - m);
    float e_face = std::exp(face_logit - m);
    return e_face / (e_bg + e_face);
}

struct Candidate {
    Detection det;
    // Keep separately for sort
};

inline float iou(const Detection& a, const Detection& b) {
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return (uni > 0) ? inter / uni : 0.0f;
}

} // namespace

void retinaface_postprocess(const float* bbox_out,
                            const float* cls_out,
                            const float* lmk_out,
                            const std::vector<Anchor>& anchors,
                            const PreInfo& pre,
                            float conf_thresh,
                            float nms_thresh,
                            std::vector<Detection>& out) {
    out.clear();
    const int   N          = static_cast<int>(anchors.size());
    const float var0       = 0.1f;   // variances[0]
    const float var1       = 0.2f;   // variances[1]
    const float input_size = static_cast<float>(pre.dst_w);   // 320

    std::vector<Detection> cands;
    cands.reserve(64);

    for (int i = 0; i < N; ++i) {
        float bg   = cls_out[i * 2 + 0];
        float face = cls_out[i * 2 + 1];
        float score = softmax_face_prob(bg, face);
        if (score < conf_thresh) continue;

        const Anchor& a  = anchors[i];
        const float* loc = &bbox_out[i * 4];
        const float* lmk = &lmk_out[i * 10];

        // Decode bbox (RetinaFace convention, center form → corner form)
        // in normalized [0,1] letterbox coords
        float cx = a.cx + loc[0] * var0 * a.sx;
        float cy = a.cy + loc[1] * var0 * a.sy;
        float w  = a.sx * std::exp(loc[2] * var1);
        float h  = a.sy * std::exp(loc[3] * var1);
        float x1 = cx - w * 0.5f;
        float y1 = cy - h * 0.5f;
        float x2 = cx + w * 0.5f;
        float y2 = cy + h * 0.5f;

        // → letterbox pixel coords (320×320)
        x1 *= input_size; y1 *= input_size;
        x2 *= input_size; y2 *= input_size;

        // Undo letterbox: subtract pad, divide by scale → original image coords
        Detection d;
        d.score = score;
        d.x1 = (x1 - pre.pad_x) / pre.scale;
        d.y1 = (y1 - pre.pad_y) / pre.scale;
        d.x2 = (x2 - pre.pad_x) / pre.scale;
        d.y2 = (y2 - pre.pad_y) / pre.scale;

        // Decode 5 landmarks similarly (all share var0 in RetinaFace)
        for (int k = 0; k < 5; ++k) {
            float px = a.cx + lmk[k * 2 + 0] * var0 * a.sx;
            float py = a.cy + lmk[k * 2 + 1] * var0 * a.sy;
            px *= input_size;
            py *= input_size;
            d.landmarks[k * 2 + 0] = (px - pre.pad_x) / pre.scale;
            d.landmarks[k * 2 + 1] = (py - pre.pad_y) / pre.scale;
        }

        cands.push_back(d);
    }

    if (cands.empty()) return;

    // NMS: sort by score desc, greedily suppress overlaps
    std::sort(cands.begin(), cands.end(),
              [](const Detection& a, const Detection& b){ return a.score > b.score; });

    std::vector<char> suppressed(cands.size(), 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (suppressed[i]) continue;
        out.push_back(cands[i]);
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (suppressed[j]) continue;
            if (iou(cands[i], cands[j]) > nms_thresh) {
                suppressed[j] = 1;
            }
        }
    }
}
