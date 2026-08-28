#pragma once
// SCRFD 2.5g_bnkps postprocess.
//
// Model outputs 9 tensors organized as 3 FPN levels (strides 8/16/32),
// each level producing (score, bbox_distances, kps_distances). Anchors are
// implicit — each feature-map location has 2 anchors sharing the same center.
//
// Decode: bbox = distance form (l, t, r, b) from anchor center, scaled by
// stride. Landmarks = per-keypoint (dx, dy) distance from anchor center.
//
// Preprocess is identical to RetinaFace 640 (letterbox 640×640 BGR uint8).

#include <vector>
#include "detect_pre.h"        // PreInfo
#include "detection.h"         // Detection struct

// Forward-declare awnn context to avoid pulling awnn headers into this header
struct Awnn_Context;
typedef struct Awnn_Context Awnn_Context_t;

class ScrfdDecoder {
public:
    static constexpr int kNumLevels     = 3;
    static constexpr int kAnchorsPerLoc = 2;   // bnkps variant → 2 anchors/loc

    // Inspect model outputs and configure the mapping (level, kind) → tensor
    // index. Returns false if the model is not a recognized SCRFD bnkps.
    // Also prints tensor layout for diagnostic.
    bool init(Awnn_Context_t* ctx, int input_size);

    // Decode raw NPU outputs + NMS into detections in ORIGINAL frame coords.
    // `outputs` is what awnn_get_output_buffers() returns.
    void decode(float** outputs,
                const PreInfo& pre,
                float score_thresh,
                float nms_thresh,
                std::vector<Detection>& out) const;

    int  input_size() const { return input_size_; }
    bool ready()      const { return initialized_; }

private:
    int  input_size_       = 0;
    int  score_idx_[kNumLevels] = {-1, -1, -1};
    int  bbox_idx_[kNumLevels]  = {-1, -1, -1};
    int  kps_idx_[kNumLevels]   = {-1, -1, -1};
    bool initialized_      = false;
};
