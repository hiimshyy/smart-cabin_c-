// enroll_faces: standalone tool to build a face identity DB (.fdb) from a
// folder of images. Layout:
//   <root>/
//     alice/1.jpg 2.jpg ...
//     bob/1.jpg   ...
// One embedding per image is extracted, averaged per subfolder → 1 identity.
//
// Usage:
//   enroll_faces --dir <root> --out <out.fdb> \
//                --det-model <retinaface.nb> --recog-model <recog.nb> \
//                [--recog-dim N] [--recog-bgr] [--min-face-px N]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <awnn_lib.h>

#include "anchors.h"
#include "retinaface_pre.h"
#include "retinaface_post.h"
#include "face_align.h"
#include "face_recog.h"
#include "face_db.h"

namespace fs = std::filesystem;

static constexpr int NPU_INPUT_W = 320;
static constexpr int NPU_INPUT_H = 320;

// Detect largest face in an image. Returns empty vector if no face.
static void detect_largest_face(Awnn_Context_t* ctx,
                                const std::vector<Anchor>& anchors,
                                const cv::Mat& img_bgr,
                                std::vector<uint8_t>& input_buf,
                                Detection& out_best,
                                bool& found) {
    found = false;
    PreInfo pre;
    retinaface_preprocess(img_bgr, input_buf.data(),
                          NPU_INPUT_W, NPU_INPUT_H, pre);

    void* ins[] = { input_buf.data() };
    awnn_set_input_buffers(ctx, ins);
    awnn_run(ctx);
    float** outs = awnn_get_output_buffers(ctx);

    std::vector<Detection> dets;
    retinaface_postprocess(outs[0], outs[1], outs[2],
                           anchors, pre, 0.5f, 0.4f, dets);
    if (dets.empty()) return;

    // Pick largest area
    auto area = [](const Detection& d) {
        return std::max(0.0f, d.x2 - d.x1) * std::max(0.0f, d.y2 - d.y1);
    };
    auto it = std::max_element(dets.begin(), dets.end(),
        [&](const Detection& a, const Detection& b){ return area(a) < area(b); });
    out_best = *it;
    found = true;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --dir <root> --out <out.fdb>\n"
        "           --det-model <retinaface.nb>\n"
        "           --recog-model <recog.nb>\n"
        "           [--recog-dim N (default 512)]\n"
        "           [--recog-bgr    (feed BGR, default RGB)]\n"
        "           [--min-face-px N (skip small faces, default 40)]\n",
        prog);
}

int main(int argc, char** argv) {
    std::string dir_path, out_path;
    std::string det_model, recog_model;
    int   recog_dim   = 512;
    bool  recog_rgb   = true;
    int   min_face_px = 40;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--dir")         dir_path    = next("--dir");
        else if (a == "--out")         out_path    = next("--out");
        else if (a == "--det-model")   det_model   = next("--det-model");
        else if (a == "--recog-model") recog_model = next("--recog-model");
        else if (a == "--recog-dim")   recog_dim   = std::atoi(next("--recog-dim"));
        else if (a == "--recog-bgr")   recog_rgb   = false;
        else if (a == "--min-face-px") min_face_px = std::atoi(next("--min-face-px"));
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    }
    if (dir_path.empty() || out_path.empty() ||
        det_model.empty() || recog_model.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    if (!fs::is_directory(dir_path)) {
        fprintf(stderr, "not a directory: %s\n", dir_path.c_str());
        return 1;
    }

    printf("[enroll] dir=%s  out=%s\n", dir_path.c_str(), out_path.c_str());
    printf("[enroll] det=%s\n", det_model.c_str());
    printf("[enroll] recog=%s (dim=%d rgb=%d)\n",
           recog_model.c_str(), recog_dim, recog_rgb ? 1 : 0);

    awnn_init();
    Awnn_Context_t* det_ctx = awnn_create(det_model.c_str());
    if (!det_ctx) {
        fprintf(stderr, "[enroll] failed to load detection model\n");
        awnn_uninit();
        return 2;
    }

    FaceRecognizer* recognizer = new FaceRecognizer(recog_model, recog_dim, recog_rgb);

    auto anchors = generate_retinaface_anchors(NPU_INPUT_W);
    std::vector<uint8_t> input_buf(NPU_INPUT_W * NPU_INPUT_H * 3);

    FaceDB db;
    db.set_dim(recog_dim);

    int total_persons = 0, total_imgs = 0, total_used = 0;

    for (const auto& person_entry : fs::directory_iterator(dir_path)) {
        if (!person_entry.is_directory()) continue;
        std::string person = person_entry.path().filename().string();
        std::vector<std::vector<float>> embs;

        for (const auto& img_entry : fs::directory_iterator(person_entry.path())) {
            if (!img_entry.is_regular_file()) continue;
            std::string ext = img_entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".jpg" && ext != ".jpeg" && ext != ".png" && ext != ".bmp")
                continue;
            ++total_imgs;

            cv::Mat img = cv::imread(img_entry.path().string(), cv::IMREAD_COLOR);
            if (img.empty()) {
                fprintf(stderr, "  [%s] cannot read %s\n", person.c_str(),
                        img_entry.path().c_str());
                continue;
            }

            Detection best; bool found = false;
            detect_largest_face(det_ctx, anchors, img, input_buf, best, found);
            if (!found) {
                fprintf(stderr, "  [%s] no face in %s\n", person.c_str(),
                        img_entry.path().filename().string().c_str());
                continue;
            }
            float fw = best.x2 - best.x1, fh = best.y2 - best.y1;
            if (fw < min_face_px || fh < min_face_px) {
                fprintf(stderr, "  [%s] face too small (%.0fx%.0f) in %s\n",
                        person.c_str(), fw, fh,
                        img_entry.path().filename().string().c_str());
                continue;
            }

            cv::Mat aligned;
            align_face_112(img, best.landmarks, aligned);

            std::vector<float> e;
            if (!recognizer->extract(aligned, e)) continue;
            embs.push_back(std::move(e));
            ++total_used;
        }

        if (!embs.empty()) {
            db.add(person, embs);
            ++total_persons;
            printf("  + %s (%zu embeddings averaged)\n", person.c_str(), embs.size());
        } else {
            printf("  - %s: NO usable embedding (skipped)\n", person.c_str());
        }
    }

    printf("[enroll] persons=%d, imgs_scanned=%d, imgs_used=%d\n",
           total_persons, total_imgs, total_used);
    if (db.size() == 0) {
        fprintf(stderr, "[enroll] DB empty, not saving\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 3;
    }
    if (!db.save(out_path)) {
        fprintf(stderr, "[enroll] save failed: %s\n", out_path.c_str());
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 4;
    }
    printf("[enroll] saved %zu identities → %s\n", db.size(), out_path.c_str());

    delete recognizer;
    awnn_destroy(det_ctx);
    awnn_uninit();
    return 0;
}
