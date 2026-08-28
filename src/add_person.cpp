// add_person: append/update a single identity in an existing .fdb DB
// without rebuilding the full database. Faster than rerunning enroll_faces.
//
// Usage:
//   ./add_person --name X --image img1.jpg [--image img2.jpg ...]
//                --db faces.fdb
//                --det-model <scrfd.nb>
//                --recog-model <recog.nb>
//                [--recog-dim N] [--recog-bgr]
//                [--replace | --merge]
//                [--min-face-px N (default 40)]

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

#include "detect_pre.h"
#include "detection.h"
#include "scrfd_post.h"
#include "face_align.h"
#include "face_recog.h"
#include "face_db.h"

namespace fs = std::filesystem;

static constexpr int NPU_INPUT_W = 640;
static constexpr int NPU_INPUT_H = 640;

static bool detect_largest_face(Awnn_Context_t* ctx,
                                const ScrfdDecoder& scrfd,
                                const cv::Mat& img_bgr,
                                std::vector<uint8_t>& input_buf,
                                Detection& out_best) {
    PreInfo pre;
    detect_preprocess(img_bgr, input_buf.data(),
                      NPU_INPUT_W, NPU_INPUT_H, pre);
    void* ins[] = { input_buf.data() };
    awnn_set_input_buffers(ctx, ins);
    awnn_run(ctx);
    float** outs = awnn_get_output_buffers(ctx);

    std::vector<Detection> dets;
    scrfd.decode(outs, pre, 0.5f, 0.4f, dets);
    if (dets.empty()) return false;

    auto area = [](const Detection& d) {
        return std::max(0.0f, d.x2 - d.x1) * std::max(0.0f, d.y2 - d.y1);
    };
    auto it = std::max_element(dets.begin(), dets.end(),
        [&](const Detection& a, const Detection& b){ return area(a) < area(b); });
    out_best = *it;
    return true;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --name X --image img1.jpg [--image img2.jpg ...]\n"
        "           --db faces.fdb\n"
        "           --det-model <scrfd.nb>\n"
        "           --recog-model <recog.nb>\n"
        "           [--recog-dim N (default 512)]\n"
        "           [--recog-bgr    (feed BGR, default RGB)]\n"
        "           [--replace]     (overwrite if name exists)\n"
        "           [--merge]       (average with existing if name exists)\n"
        "           [--min-face-px N (default 40)]\n",
        prog);
}

int main(int argc, char** argv) {
    std::string name;
    std::vector<std::string> images;
    std::string db_path;
    std::string det_model, recog_model;
    int   recog_dim   = 512;
    bool  recog_rgb   = true;
    int   min_face_px = 40;
    bool  do_replace  = false;
    bool  do_merge    = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", w); exit(2); }
            return argv[++i];
        };
        if      (a == "--name")         name        = next("--name");
        else if (a == "--image")        images.push_back(next("--image"));
        else if (a == "--db")           db_path     = next("--db");
        else if (a == "--det-model")    det_model   = next("--det-model");
        else if (a == "--recog-model")  recog_model = next("--recog-model");
        else if (a == "--recog-dim")    recog_dim   = std::atoi(next("--recog-dim"));
        else if (a == "--recog-bgr")    recog_rgb   = false;
        else if (a == "--replace")      do_replace  = true;
        else if (a == "--merge")        do_merge    = true;
        else if (a == "--min-face-px")  min_face_px = std::atoi(next("--min-face-px"));
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown arg: %s\n", a.c_str());
            print_usage(argv[0]); return 2;
        }
    }
    if (name.empty() || images.empty() || db_path.empty() ||
        det_model.empty() || recog_model.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    if (do_replace && do_merge) {
        fprintf(stderr, "use --replace OR --merge, not both\n");
        return 2;
    }

    printf("[add] name=%s  db=%s  #images=%zu  input=%dx%d\n",
           name.c_str(), db_path.c_str(), images.size(),
           NPU_INPUT_W, NPU_INPUT_H);

    // ---- Load / init DB ------------------------------------------------
    FaceDB db;
    bool db_existed = fs::exists(db_path);
    if (db_existed) {
        if (!db.load(db_path)) {
            fprintf(stderr, "[add] cannot load existing db %s\n", db_path.c_str());
            return 3;
        }
        printf("[add] loaded existing db: %zu identities, dim=%d\n",
               db.size(), db.dim());
        if (db.dim() != recog_dim) {
            fprintf(stderr, "[add] dim mismatch: db=%d vs --recog-dim=%d\n",
                    db.dim(), recog_dim);
            return 3;
        }
    } else {
        db.set_dim(recog_dim);
        printf("[add] db does not exist, will create new one\n");
    }

    int existing_idx = db.find(name);
    std::vector<float> existing_embedding;
    if (existing_idx >= 0) {
        if (!do_replace && !do_merge) {
            fprintf(stderr,
                "[add] identity '%s' already exists (index %d).\n"
                "      Use --replace to overwrite or --merge to combine.\n",
                name.c_str(), existing_idx);
            return 4;
        }
        if (do_merge) {
            existing_embedding = db.all()[existing_idx].embedding;
            printf("[add] MERGE mode: will combine with existing embedding\n");
        } else {
            printf("[add] REPLACE mode: existing entry will be overwritten\n");
        }
        db.remove_at(static_cast<size_t>(existing_idx));
    }

    awnn_init();
    // Load recog BEFORE detect.
    FaceRecognizer* recognizer = new FaceRecognizer(recog_model, recog_dim, recog_rgb);
    if (!recognizer->model_loaded()) {
        fprintf(stderr, "[add] failed to load recog model\n");
        delete recognizer;
        awnn_uninit();
        return 5;
    }

    Awnn_Context_t* det_ctx = awnn_create(det_model.c_str());
    if (!det_ctx) {
        fprintf(stderr, "[add] failed to load detection model\n");
        delete recognizer;
        awnn_uninit();
        return 5;
    }

    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        fprintf(stderr, "[add] scrfd init failed\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 5;
    }

    std::vector<uint8_t> input_buf(NPU_INPUT_W * NPU_INPUT_H * 3);

    std::vector<std::vector<float>> new_embs;
    for (const auto& img_path : images) {
        cv::Mat img = cv::imread(img_path, cv::IMREAD_COLOR);
        if (img.empty()) {
            fprintf(stderr, "  [%s] cannot read image (skip)\n", img_path.c_str());
            continue;
        }
        Detection best;
        if (!detect_largest_face(det_ctx, scrfd, img, input_buf, best)) {
            fprintf(stderr, "  [%s] no face detected (skip)\n", img_path.c_str());
            continue;
        }
        float fw = best.x2 - best.x1, fh = best.y2 - best.y1;
        if (fw < min_face_px || fh < min_face_px) {
            fprintf(stderr, "  [%s] face too small %.0fx%.0f (skip)\n",
                    img_path.c_str(), fw, fh);
            continue;
        }

        cv::Mat aligned;
        align_face_112(img, best.landmarks, aligned);

        std::vector<float> e;
        if (!recognizer->extract(aligned, e)) {
            fprintf(stderr, "  [%s] embedding extract failed\n", img_path.c_str());
            continue;
        }
        new_embs.push_back(std::move(e));
        printf("  + %s → embedding OK\n", img_path.c_str());
    }

    if (new_embs.empty()) {
        fprintf(stderr, "[add] no usable embedding extracted. Aborting.\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 6;
    }

    if (!existing_embedding.empty()) {
        new_embs.push_back(existing_embedding);
    }

    db.add(name, new_embs);
    printf("[add] identity '%s' now has embedding averaged from %zu sources\n",
           name.c_str(), new_embs.size());

    if (!db.save(db_path)) {
        fprintf(stderr, "[add] failed to save db %s\n", db_path.c_str());
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 7;
    }
    printf("[add] saved db: %zu total identities → %s\n",
           db.size(), db_path.c_str());

    delete recognizer;
    awnn_destroy(det_ctx);
    awnn_uninit();
    return 0;
}
