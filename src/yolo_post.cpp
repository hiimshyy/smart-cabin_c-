#include "yolo_post.h"

#include <awnn_lib.h>
#define time_begin _yolo_time_begin_unused
#define time_end   _yolo_time_end_unused
#include <awnn_internal.h>
#undef time_begin
#undef time_end

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int   kStrides[3] = {8, 16, 32};

// Standard YOLOv5 COCO anchor priors, in pixel units on 640 input.
// Order MUST match kStrides. 3 anchors per level, (w, h).
constexpr float kAnchors[3][3][2] = {
    // stride 8   (small objects)
    {{ 10.f,  13.f}, { 16.f,  30.f}, { 33.f,  23.f}},
    // stride 16  (medium)
    {{ 30.f,  61.f}, { 62.f,  45.f}, { 59.f, 119.f}},
    // stride 32  (large)
    {{116.f,  90.f}, {156.f, 198.f}, {373.f, 326.f}},
};

inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// SDK exports outputs in ranges [0, 1] already OR raw logits — the produced
// .nb here contains "raw" activations (no sigmoid baked in). Empirically the
// values are in a wider range so we sigmoid them. Fall back to pass-through
// if already normalized (defensive against re-exported models).
inline float score_act(float x) {
    if (x >= 0.0f && x <= 1.0f) return x;
    return sigmoid(x);
}

inline float iou(const PersonDet& a, const PersonDet& b) {
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);
    float w   = std::max(0.0f, xx2 - xx1);
    float h   = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni    = area_a + area_b - inter;
    return (uni > 0) ? inter / uni : 0.0f;
}

} // namespace

bool YoloDecoder::init(Awnn_Context_t* ctx, int input_size) {
    if (!ctx || input_size != 640) {
        fprintf(stderr, "[yolo] init requires input_size=640 (got %d)\n", input_size);
        return false;
    }
    input_size_ = input_size;

    if (ctx->output_count != 3) {
        fprintf(stderr, "[yolo] expected 3 outputs, got %u\n", ctx->output_count);
        return false;
    }

    // Expected element counts per level (grid × grid × anchors × channels).
    int expected[3];
    for (int L = 0; L < 3; ++L) {
        int g = input_size / kStrides[L];
        expected[L] = g * g * kAnchorsPerLoc * kChannels;
    }
    printf("[yolo] inspecting outputs (input=%d, anchors=%d, channels=%d):\n",
           input_size, kAnchorsPerLoc, kChannels);
    for (unsigned int i = 0; i < ctx->output_count; ++i) {
        const auto& p = ctx->output_params[i];
        printf("  out[%u] '%s' elements=%u\n", i, p.name, p.elements);
    }

    // Match by element count: each level has unique count.
    for (unsigned int i = 0; i < ctx->output_count; ++i) {
        int e = (int)ctx->output_params[i].elements;
        for (int L = 0; L < 3; ++L) {
            if (e == expected[L] && level_out_idx_[L] < 0) {
                level_out_idx_[L] = (int)i;
                break;
            }
        }
    }
    for (int L = 0; L < 3; ++L) {
        if (level_out_idx_[L] < 0) {
            fprintf(stderr, "[yolo] no output tensor matches level %d "
                    "(stride %d, expected %d elements)\n",
                    L, kStrides[L], expected[L]);
            return false;
        }
    }

    printf("[yolo] output mapping:\n");
    for (int L = 0; L < 3; ++L) {
        printf("  stride %2d  -> out[%d]  grid %dx%d  elements=%u\n",
               kStrides[L], level_out_idx_[L],
               input_size / kStrides[L], input_size / kStrides[L],
               ctx->output_params[level_out_idx_[L]].elements);
    }
    initialized_ = true;
    return true;
}

void YoloDecoder::decode(float** outputs,
                         const PreInfo& pre,
                         float score_thresh,
                         float nms_thresh,
                         std::vector<PersonDet>& out) const {
    out.clear();
    if (!initialized_ || !outputs) return;

    std::vector<PersonDet> cands;
    cands.reserve(128);

    for (int L = 0; L < kNumLevels; ++L) {
        int stride = kStrides[L];
        int gH = input_size_ / stride;
        int gW = input_size_ / stride;
        const float* buf = outputs[level_out_idx_[L]];
        if (!buf) continue;

        // Memory layout inferred from sizes = [W, H, 85, 3, 1]:
        // Fastest-varying = W (grid x). Traversal order for a full sweep:
        //   for anchor a: for channel c: for gy: for gx:
        // Total per (anchor, channel) block = gW*gH. Adjacent scalars in memory
        // are gx-adjacent within one (a, c) block.
        //
        // Index formula:
        //   offset(gy, gx, c, a) = a * (kChannels * gH * gW)
        //                       + c * (gH * gW)
        //                       + gy * gW
        //                       + gx
        //
        // NOTE: We couldn't verify this empirically until visually inspecting
        // bbox — decode is written to match the standard ultralytics ONNX
        // export order after Acuity conversion (a-first, then c, then spatial).

        auto get = [&](int gy, int gx, int c, int a) -> float {
            return buf[a * (kChannels * gH * gW)
                     + c * (gH * gW)
                     + gy * gW
                     + gx];
        };

        for (int a = 0; a < kAnchorsPerLoc; ++a) {
            float aw = kAnchors[L][a][0];
            float ah = kAnchors[L][a][1];
            for (int gy = 0; gy < gH; ++gy) {
                for (int gx = 0; gx < gW; ++gx) {
                    // Objectness first — early exit if low.
                    float obj = score_act(get(gy, gx, 4, a));
                    if (obj < score_thresh) continue;

                    // Class 0 (person). If person score * obj < threshold, skip.
                    float cls0 = score_act(get(gy, gx, 5, a));
                    float score = obj * cls0;
                    if (score < score_thresh) continue;

                    // Optional: check other classes aren't higher (skip if this
                    // detection is more like a bicycle etc). For 1-class use
                    // case we skip this — false positives get filtered by NMS.

                    // Bbox decode
                    float rx = get(gy, gx, 0, a);
                    float ry = get(gy, gx, 1, a);
                    float rw = get(gy, gx, 2, a);
                    float rh = get(gy, gx, 3, a);
                    float sx = sigmoid(rx) * 2.0f - 0.5f;
                    float sy = sigmoid(ry) * 2.0f - 0.5f;
                    float sw = sigmoid(rw) * 2.0f; sw = sw * sw;
                    float sh = sigmoid(rh) * 2.0f; sh = sh * sh;

                    // In letterbox pixel coords
                    float cx = (gx + sx) * stride;
                    float cy = (gy + sy) * stride;
                    float bw = aw * sw;
                    float bh = ah * sh;
                    float x1 = cx - bw * 0.5f;
                    float y1 = cy - bh * 0.5f;
                    float x2 = cx + bw * 0.5f;
                    float y2 = cy + bh * 0.5f;

                    // Invert letterbox to ORIGINAL frame coords
                    PersonDet d;
                    d.score = score;
                    d.x1 = (x1 - pre.pad_x) / pre.scale;
                    d.y1 = (y1 - pre.pad_y) / pre.scale;
                    d.x2 = (x2 - pre.pad_x) / pre.scale;
                    d.y2 = (y2 - pre.pad_y) / pre.scale;
                    cands.push_back(d);
                }
            }
        }
    }

    if (cands.empty()) return;

    std::sort(cands.begin(), cands.end(),
              [](const PersonDet& a, const PersonDet& b){ return a.score > b.score; });
    std::vector<char> suppressed(cands.size(), 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (suppressed[i]) continue;
        out.push_back(cands[i]);
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (suppressed[j]) continue;
            if (iou(cands[i], cands[j]) > nms_thresh) suppressed[j] = 1;
        }
    }
}
