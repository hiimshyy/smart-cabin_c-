#include "scrfd_post.h"

#include <awnn_lib.h>
// awnn_internal.h defines file-scope globals `time_begin`/`time_end` as
// non-static — including it from >1 TU causes multiple-definition link
// errors. Rename them in our TU so they don't collide with awnn_lib.o.
#define time_begin _scrfd_time_begin_unused
#define time_end   _scrfd_time_end_unused
#include <awnn_internal.h>
#undef time_begin
#undef time_end

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kStrides[3] = {8, 16, 32};

// SCRFD ONNX from insightface bakes sigmoid before the score output.
// Values should already be in [0,1]. Fall back to sigmoid if raw logits.
inline float score_activation(float x) {
    if (x >= 0.0f && x <= 1.0f) return x;
    return 1.0f / (1.0f + std::exp(-x));
}

inline float iou(const Detection& a, const Detection& b) {
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

// Match tensor name substring, e.g. name contains "score" AND "8" (as full token)
inline bool name_matches(const char* name, const char* kind_key, int stride) {
    if (!name || !*name) return false;
    if (!std::strstr(name, kind_key)) return false;
    char sbuf[16];
    std::snprintf(sbuf, sizeof(sbuf), "%d", stride);
    // Naive substring: acceptable because the three strides (8/16/32) don't
    // overlap textually with each other (8 is not substring of 16 or 32).
    return std::strstr(name, sbuf) != nullptr;
}

} // namespace

bool ScrfdDecoder::init(Awnn_Context_t* ctx, int input_size) {
    if (!ctx || input_size <= 0) return false;
    input_size_ = input_size;

    unsigned int n = ctx->output_count;
    if (n != 9) {
        fprintf(stderr, "[scrfd] expected 9 output tensors, got %u\n", n);
        return false;
    }

    const int W = input_size;
    const int expected_score[3] = {
        (W/8)*(W/8) * kAnchorsPerLoc * 1,
        (W/16)*(W/16) * kAnchorsPerLoc * 1,
        (W/32)*(W/32) * kAnchorsPerLoc * 1,
    };
    const int expected_bbox[3] = {
        (W/8)*(W/8) * kAnchorsPerLoc * 4,
        (W/16)*(W/16) * kAnchorsPerLoc * 4,
        (W/32)*(W/32) * kAnchorsPerLoc * 4,
    };
    const int expected_kps[3] = {
        (W/8)*(W/8) * kAnchorsPerLoc * 10,
        (W/16)*(W/16) * kAnchorsPerLoc * 10,
        (W/32)*(W/32) * kAnchorsPerLoc * 10,
    };

    printf("[scrfd] inspecting %u output tensors (input=%dx%d, na=%d)\n",
           n, input_size, input_size, kAnchorsPerLoc);
    for (unsigned int i = 0; i < n; ++i) {
        const auto& p = ctx->output_params[i];
        printf("  [%u] name='%s' elements=%u\n", i, p.name, p.elements);
    }

    // ---- Strategy 1: classify by tensor name ---------------------------
    bool by_name_ok = true;
    int s_by_name[3] = {-1,-1,-1};
    int b_by_name[3] = {-1,-1,-1};
    int k_by_name[3] = {-1,-1,-1};
    for (unsigned int i = 0; i < n; ++i) {
        const char* nm = ctx->output_params[i].name;
        bool matched = false;
        for (int L = 0; L < 3; ++L) {
            if (name_matches(nm, "score", kStrides[L]) && s_by_name[L] < 0) {
                s_by_name[L] = (int)i; matched = true; break;
            }
            if (name_matches(nm, "bbox",  kStrides[L]) && b_by_name[L] < 0) {
                b_by_name[L] = (int)i; matched = true; break;
            }
            if (name_matches(nm, "kps",   kStrides[L]) && k_by_name[L] < 0) {
                k_by_name[L] = (int)i; matched = true; break;
            }
        }
        if (!matched) { by_name_ok = false; break; }
    }
    if (by_name_ok) {
        for (int L = 0; L < 3; ++L) {
            if (s_by_name[L] < 0 || b_by_name[L] < 0 || k_by_name[L] < 0) {
                by_name_ok = false; break;
            }
        }
    }

    if (by_name_ok) {
        for (int L = 0; L < 3; ++L) {
            score_idx_[L] = s_by_name[L];
            bbox_idx_[L]  = b_by_name[L];
            kps_idx_[L]   = k_by_name[L];
        }
        printf("[scrfd] classified outputs by tensor NAME\n");
    } else {
        // ---- Strategy 2: element counts + canonical order ------------
        // Element counts (input=640, na=2):
        //   kps-8=128000  kps-16=32000  kps-32=8000
        //   bbox-8=51200  bbox-16=12800  bbox-32=3200
        //   score-8=12800  score-16=3200  score-32=800
        //
        // Unique: kps-8/16/32, bbox-8, score-32.
        // Collisions:
        //   12800: {score-8, bbox-16}  → within collision, score comes
        //                                before bbox in canonical output order
        //    3200: {score-16, bbox-32} → ditto
        printf("[scrfd] name classification failed; using element-count + order\n");

        for (unsigned int i = 0; i < n; ++i) {
            int e = (int)ctx->output_params[i].elements;
            // KPS (unique)
            for (int L = 0; L < 3; ++L) {
                if (e == expected_kps[L] && kps_idx_[L] < 0) { kps_idx_[L] = (int)i; goto next; }
            }
            // BBOX-8 unique 51200
            if (e == expected_bbox[0] && bbox_idx_[0] < 0) { bbox_idx_[0] = (int)i; goto next; }
            // SCORE-32 unique 800
            if (e == expected_score[2] && score_idx_[2] < 0) { score_idx_[2] = (int)i; goto next; }
            // Collisions: within same size, first occurrence = score, second = bbox
            if (e == expected_score[0] /* == expected_bbox[1] == 12800 */) {
                if      (score_idx_[0] < 0) score_idx_[0] = (int)i;
                else if (bbox_idx_[1]  < 0) bbox_idx_[1]  = (int)i;
                goto next;
            }
            if (e == expected_score[1] /* == expected_bbox[2] == 3200 */) {
                if      (score_idx_[1] < 0) score_idx_[1] = (int)i;
                else if (bbox_idx_[2]  < 0) bbox_idx_[2]  = (int)i;
                goto next;
            }
            fprintf(stderr, "[scrfd] output %u element count %d not recognized\n", i, e);
            return false;
        next: ;
        }
    }

    // ---- Verify complete mapping and element counts match -----------
    for (int L = 0; L < 3; ++L) {
        if (score_idx_[L] < 0 || bbox_idx_[L] < 0 || kps_idx_[L] < 0) {
            fprintf(stderr, "[scrfd] failed to identify all 9 outputs (level %d)\n", L);
            return false;
        }
        int se = (int)ctx->output_params[score_idx_[L]].elements;
        int be = (int)ctx->output_params[bbox_idx_[L]].elements;
        int ke = (int)ctx->output_params[kps_idx_[L]].elements;
        if (se != expected_score[L] || be != expected_bbox[L] || ke != expected_kps[L]) {
            fprintf(stderr,
                    "[scrfd] level %d (stride %d) shape mismatch: "
                    "score=%d/%d bbox=%d/%d kps=%d/%d\n",
                    L, kStrides[L], se, expected_score[L],
                    be, expected_bbox[L], ke, expected_kps[L]);
            return false;
        }
    }

    printf("[scrfd] output mapping OK:\n");
    for (int L = 0; L < 3; ++L) {
        printf("  stride %2d  score=out[%d]  bbox=out[%d]  kps=out[%d]\n",
               kStrides[L], score_idx_[L], bbox_idx_[L], kps_idx_[L]);
    }
    initialized_ = true;
    return true;
}

void ScrfdDecoder::decode(float** outputs,
                          const PreInfo& pre,
                          float score_thresh,
                          float nms_thresh,
                          std::vector<Detection>& out) const {
    out.clear();
    if (!initialized_ || !outputs) return;

    std::vector<Detection> cands;
    cands.reserve(64);

    for (int L = 0; L < kNumLevels; ++L) {
        int stride = kStrides[L];
        int fH = input_size_ / stride;
        int fW = input_size_ / stride;
        const float* score = outputs[score_idx_[L]];
        const float* bbox  = outputs[bbox_idx_[L]];
        const float* kps   = outputs[kps_idx_[L]];
        if (!score || !bbox || !kps) continue;

        int idx = 0;
        for (int fy = 0; fy < fH; ++fy) {
            for (int fx = 0; fx < fW; ++fx) {
                // Both anchors at a location share the same center.
                float cx = static_cast<float>(fx) * stride;
                float cy = static_cast<float>(fy) * stride;

                for (int a = 0; a < kAnchorsPerLoc; ++a, ++idx) {
                    float s = score_activation(score[idx]);
                    if (s < score_thresh) continue;

                    const float* bp = bbox + idx * 4;
                    float l = bp[0] * stride;
                    float t = bp[1] * stride;
                    float r = bp[2] * stride;
                    float b = bp[3] * stride;

                    // Letterbox pixel coords → original image coords
                    float x1 = cx - l;
                    float y1 = cy - t;
                    float x2 = cx + r;
                    float y2 = cy + b;

                    Detection d;
                    d.score = s;
                    d.x1 = (x1 - pre.pad_x) / pre.scale;
                    d.y1 = (y1 - pre.pad_y) / pre.scale;
                    d.x2 = (x2 - pre.pad_x) / pre.scale;
                    d.y2 = (y2 - pre.pad_y) / pre.scale;

                    const float* kp = kps + idx * 10;
                    for (int k = 0; k < 5; ++k) {
                        float lx = cx + kp[k*2 + 0] * stride;
                        float ly = cy + kp[k*2 + 1] * stride;
                        d.landmarks[k*2 + 0] = (lx - pre.pad_x) / pre.scale;
                        d.landmarks[k*2 + 1] = (ly - pre.pad_y) / pre.scale;
                    }
                    cands.push_back(d);
                }
            }
        }
    }

    if (cands.empty()) return;

    std::sort(cands.begin(), cands.end(),
              [](const Detection& a, const Detection& b){ return a.score > b.score; });
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
