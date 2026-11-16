#include "yolo_post.h"

#include <awnn_lib.h>
#define time_begin _test_yolo_time_begin_unused
#define time_end _test_yolo_time_end_unused
#include <awnn_internal.h>
#undef time_begin
#undef time_end

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int checks = 0, failed = 0;
#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

namespace {

constexpr int kInputSize    = 640;
constexpr int kGridSizes[3] = {80, 40, 20};

struct SyntheticYolo {
    Awnn_params_t      params[3]{};
    Awnn_Context_t     ctx{};
    std::vector<float> bufs[3];
    float*             ptrs[3]{};

    SyntheticYolo() {
        // Store outputs in reverse order (stride 32, 16, 8) to exercise init() matching.
        for (int i = 0; i < 3; ++i) {
            const int g = kGridSizes[2 - i];
            const int n = g * g * YoloDecoder::kAnchorsPerLoc * YoloDecoder::kChannels;
            bufs[i].assign(n, 0.0f);
            ptrs[i]            = bufs[i].data();
            params[i].elements = static_cast<vip_uint32_t>(n);
        }
        ctx.output_count  = 3;
        ctx.output_params = params;
    }

    void clear_buffers() {
        for (auto& b : bufs)
            std::fill(b.begin(), b.end(), 0.0f);
    }

    // `level` uses YoloDecoder ordering: 0=stride8 (80x80), 1=stride16 (40x40), 2=stride32 (20x20).
    void set_cell(int level, int anchor, int gy, int gx, int channel, float value) {
        const int out_idx = 2 - level;
        const int g       = kGridSizes[level];
        const int idx = anchor * (YoloDecoder::kChannels * g * g) + channel * (g * g) + gy * g + gx;
        bufs[out_idx][idx] = value;
    }
};

} // namespace

int main() {
    CHECK(std::strcmp(coco_animal_name(YoloDecoder::kCocoCat), "cat") == 0,
          "coco_animal_name maps 15 to cat");
    CHECK(std::strcmp(coco_animal_name(YoloDecoder::kCocoDog), "dog") == 0,
          "coco_animal_name maps 16 to dog");
    CHECK(std::strcmp(coco_animal_name(0), "animal") == 0,
          "coco_animal_name falls back to animal for person");
    CHECK(std::strcmp(coco_animal_name(-1), "animal") == 0,
          "coco_animal_name falls back to animal for unknown id");

    SyntheticYolo syn;
    YoloDecoder   yolo;
    CHECK(yolo.init(&syn.ctx, kInputSize), "YoloDecoder initializes with 3 synthetic outputs");

    const PreInfo          identity_pre{640, 640, 640, 640, 0, 0, 1.0f};
    std::vector<PersonDet> persons;
    std::vector<AnimalDet> animals;

    // 1. Arg-max filtering: person > dog must NOT emit an animal; chair > cat must NOT emit;
    //    dog as arg-max must emit dog and NOT leak into person decode.
    syn.clear_buffers();
    syn.set_cell(2, 0, 4, 4, 4, 0.90f);
    syn.set_cell(2, 0, 4, 4, 5 + 0, 0.92f);
    syn.set_cell(2, 0, 4, 4, 5 + YoloDecoder::kCocoDog, 0.70f);

    syn.set_cell(2, 0, 8, 8, 4, 0.90f);
    syn.set_cell(2, 0, 8, 8, 5 + YoloDecoder::kCocoCat, 0.75f);
    syn.set_cell(2, 0, 8, 8, 5 + 56, 0.85f); // chair > cat (class index > kCocoCat)

    syn.set_cell(2, 0, 12, 12, 4, 0.90f);
    syn.set_cell(2, 0, 12, 12, 5 + 0, 0.20f);
    syn.set_cell(2, 0, 12, 12, 5 + YoloDecoder::kCocoDog, 0.80f);

    yolo.decode(syn.ptrs, identity_pre, 0.5f, 0.45f, persons);
    yolo.decode_animals(syn.ptrs, identity_pre, 0.5f, 0.45f, animals);
    CHECK(persons.size() == 1, "person decoder finds only the person cell");
    CHECK(animals.size() == 1 && animals[0].class_id == YoloDecoder::kCocoDog &&
              std::fabs(animals[0].score - 0.72f) < 1e-4f,
          "decode_animals ignores person/chair cells and emits winning dog");

    // 2. Tie-breaking matches strict `>` 0..79 scan:
    //    person == cat -> person wins (ignored by decode_animals).
    //    cat == dog -> cat wins (class 15 < class 16).
    syn.clear_buffers();
    syn.set_cell(2, 0, 3, 3, 4, 0.90f);
    syn.set_cell(2, 0, 3, 3, 5 + 0, 0.80f);
    syn.set_cell(2, 0, 3, 3, 5 + YoloDecoder::kCocoCat, 0.80f);

    syn.set_cell(2, 0, 7, 7, 4, 0.90f);
    syn.set_cell(2, 0, 7, 7, 5 + YoloDecoder::kCocoCat, 0.80f);
    syn.set_cell(2, 0, 7, 7, 5 + YoloDecoder::kCocoDog, 0.80f);

    yolo.decode_animals(syn.ptrs, identity_pre, 0.5f, 0.45f, animals);
    CHECK(animals.size() == 1 && animals[0].class_id == YoloDecoder::kCocoCat,
          "tie-breaking keeps person-vs-cat suppressed and prefers cat over dog on tie");

    // 3. Threshold filtering + clearing stale output vector.
    syn.clear_buffers();
    syn.set_cell(2, 0, 15, 15, 4, 0.80f);
    syn.set_cell(2, 0, 15, 15, 5 + YoloDecoder::kCocoCat, 0.60f); // 0.48
    yolo.decode_animals(syn.ptrs, identity_pre, 0.5f, 0.45f, animals);
    CHECK(animals.empty(), "cat below 0.50 threshold is filtered and clears prior output");
    yolo.decode_animals(syn.ptrs, identity_pre, 0.45f, 0.45f, animals);
    CHECK(animals.size() == 1 && animals[0].class_id == YoloDecoder::kCocoCat &&
              std::fabs(animals[0].score - 0.48f) < 1e-4f,
          "cat passes lowered 0.45 threshold");

    // 4. Per-class NMS + letterbox inversion:
    //    gx=10 with sx=1.0 (sigmoid(rx)=0.75 -> rx=log(3)) and gx=11 with sx=0.0
    //    (sigmoid(rx)=0.25 -> rx=-log(3)) yield identical centers cx = 11 * 32 = 352.
    const float rx_plus  = std::log(3.0f);
    const float rx_minus = -std::log(3.0f);
    syn.clear_buffers();
    syn.set_cell(2, 0, 10, 10, 0, rx_plus);
    syn.set_cell(2, 0, 10, 10, 4, 0.90f);
    syn.set_cell(2, 0, 10, 10, 5 + YoloDecoder::kCocoDog, 0.90f); // 0.81

    syn.set_cell(2, 0, 10, 11, 0, rx_minus);
    syn.set_cell(2, 0, 10, 11, 4, 0.90f);
    syn.set_cell(2, 0, 10, 11, 5 + YoloDecoder::kCocoCat, 0.80f); // 0.72

    const PreInfo letterbox_pre{1280, 720, 640, 640, 0, 140, 0.5f};
    yolo.decode_animals(syn.ptrs, letterbox_pre, 0.5f, 0.45f, animals);
    CHECK(animals.size() == 2, "identical dog and cat boxes do not suppress each other");
    if (animals.size() == 2) {
        CHECK(animals[0].class_id == YoloDecoder::kCocoDog &&
                  animals[1].class_id == YoloDecoder::kCocoCat,
              "animals are sorted by descending score");
        // cx = 352, cy = (10+0.5)*32 = 336, bw = 116, bh = 90 in letterbox -> invert (pad_y=140,
        // scale=0.5)
        CHECK(std::fabs(animals[0].x1 - (352.0f - 58.0f) / 0.5f) < 1e-2f &&
                  std::fabs(animals[0].y1 - (336.0f - 45.0f - 140.0f) / 0.5f) < 1e-2f,
              "letterbox inversion maps animal box back to original frame coordinates");
    }

    // Two overlapping dogs at identical boxes -> lower score dog is suppressed.
    syn.clear_buffers();
    syn.set_cell(2, 0, 10, 10, 0, rx_plus);
    syn.set_cell(2, 0, 10, 10, 4, 0.90f);
    syn.set_cell(2, 0, 10, 10, 5 + YoloDecoder::kCocoDog, 0.90f); // 0.81

    syn.set_cell(2, 0, 10, 11, 0, rx_minus);
    syn.set_cell(2, 0, 10, 11, 4, 0.90f);
    syn.set_cell(2, 0, 10, 11, 5 + YoloDecoder::kCocoDog, 0.80f); // 0.72

    yolo.decode_animals(syn.ptrs, identity_pre, 0.5f, 0.45f, animals);
    CHECK(animals.size() == 1 && animals[0].class_id == YoloDecoder::kCocoDog &&
              std::fabs(animals[0].score - 0.81f) < 1e-4f,
          "overlapping same-species dog box is suppressed by per-class NMS");

    std::printf("\n[test_yolo_post] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
