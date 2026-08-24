#pragma once
// RetinaFace prior anchors generator.
// Config chuẩn cho model input 320x320 → tổng 4200 anchors.

#include <vector>
#include <cstdint>

struct Anchor {
    float cx;   // center x, normalized [0,1] theo input_size
    float cy;   // center y
    float sx;   // width  normalized
    float sy;   // height normalized
};

// Sinh danh sách 4200 anchors cho RetinaFace input 320x320.
// - Feature maps: 40x40, 20x20, 10x10
// - min_sizes: [[16,32], [64,128], [256,512]]
// - steps: [8, 16, 32]
std::vector<Anchor> generate_retinaface_anchors(int input_size = 320);
