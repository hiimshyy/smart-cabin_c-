#pragma once
// YOLOv5s COCO person postprocess.
//
// Model: yolov5s_rt_uint8_a733.nb  (Acuity export of ultralytics/yolov5s.pt)
// Input : 640×640 BGR uint8 (letterbox from arbitrary frame)
// Outputs: 3 tensors, one per FPN stride (8, 16, 32).
//   shape per level (fastest-to-slowest): sizes = [W, H, 85, 3, 1]
//   memory: 3 anchors per grid cell × 85 channels each
//   85 = 4 bbox (cx, cy, w, h in feature units) + 1 objectness + 80 COCO classes
//
// We only decode class 0 (person). Bbox format is YOLOv5 raw:
//   sx = sigmoid(raw[0]) * 2 - 0.5        (offset within cell, [-0.5, 1.5])
//   sy = sigmoid(raw[1]) * 2 - 0.5
//   sw = (sigmoid(raw[2]) * 2)^2          (scale relative to anchor)
//   sh = (sigmoid(raw[3]) * 2)^2
// Center pixel = (gx + sx) * stride, size = anchor * scale.

#include <vector>
#include "detect_pre.h"        // PreInfo

struct PersonDet {
    float x1, y1, x2, y2;    // In ORIGINAL frame coords (letterbox inverted)
    float score;             // objectness * class score, [0, 1]
};

// Forward-declare awnn context (only used by init).
struct Awnn_Context;
typedef struct Awnn_Context Awnn_Context_t;

class YoloDecoder {
public:
    static constexpr int kNumLevels     = 3;
    static constexpr int kAnchorsPerLoc = 3;
    static constexpr int kNumClasses    = 80;
    static constexpr int kChannels      = 4 + 1 + kNumClasses;   // 85

    // Verify the model has the expected 3-output YOLOv5 layout for the given
    // input size (must be 640). Auto-orders the 3 outputs by stride (largest
    // grid first). Prints tensor info.
    bool init(Awnn_Context_t* ctx, int input_size);

    // Decode + NMS. `outputs` is the array returned by awnn_get_output_buffers().
    // Only class 0 (person) is returned. Coordinates are in ORIGINAL frame
    // pixels (letterbox inverted via `pre`).
    void decode(float** outputs,
                const PreInfo& pre,
                float score_thresh,
                float nms_thresh,
                std::vector<PersonDet>& out) const;

    int  input_size() const { return input_size_; }
    bool ready()      const { return initialized_; }

private:
    int  input_size_ = 0;
    // outputs[level_out_idx_[L]] is the tensor for FPN level L (strides 8/16/32)
    int  level_out_idx_[kNumLevels] = {-1, -1, -1};
    bool initialized_ = false;
};
