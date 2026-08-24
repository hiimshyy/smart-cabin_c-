#include "anchors.h"

std::vector<Anchor> generate_retinaface_anchors(int input_size) {
    const int min_sizes[3][2] = {{16, 32}, {64, 128}, {256, 512}};
    const int steps[3]        = {8, 16, 32};
    const int num_levels      = 3;

    std::vector<Anchor> anchors;
    anchors.reserve(4200);

    const float inv_size = 1.0f / static_cast<float>(input_size);

    for (int k = 0; k < num_levels; ++k) {
        int step = steps[k];
        int fmap_h = input_size / step;
        int fmap_w = input_size / step;
        for (int i = 0; i < fmap_h; ++i) {                     // y row
            for (int j = 0; j < fmap_w; ++j) {                 // x col
                for (int m = 0; m < 2; ++m) {                   // 2 min_sizes / level
                    float s   = static_cast<float>(min_sizes[k][m]);
                    Anchor a;
                    a.cx = (j + 0.5f) * step * inv_size;
                    a.cy = (i + 0.5f) * step * inv_size;
                    a.sx = s * inv_size;
                    a.sy = s * inv_size;
                    anchors.push_back(a);
                }
            }
        }
    }
    return anchors;
}
