// enroll_faces: build the resident SQLite DB from a folder of images.
// Layout:
//   <root>/
//     alice/1.jpg 2.jpg ...
//     bob/1.jpg   ...
// Each image contributes ONE embedding row (source='id_photo') — embeddings
// are NOT averaged, so pose/lighting variation is preserved (spec R6.1).
// One resident row per subfolder (home_floor=0 sentinel = "no floor yet").
//
// Usage:
//   enroll_faces --dir <root> --db <residents.db>
//                --det-model <scrfd.nb> --recog-model <recog.nb>
//                [--recog-dim N] [--recog-bgr] [--min-face-px N]
//                [--schema db/schema.sql]

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
#include "resident_db.h"

namespace fs = std::filesystem;

static constexpr int NPU_INPUT_W = 640;
static constexpr int NPU_INPUT_H = 640;

// Detect largest face in an image using SCRFD.
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
        "Usage: %s --dir <root> --db <residents.db>\n"
        "           --det-model <scrfd.nb>\n"
        "           --recog-model <recog.nb>\n"
        "           [--recog-dim N (default 512)]\n"
        "           [--recog-bgr    (feed BGR, default RGB)]\n"
        "           [--min-face-px N (skip small faces, default 40)]\n"
        "           [--schema PATH  (schema.sql for empty DB, default db/schema.sql)]\n"
        "\n"
        "  Each image -> one embedding row (source='id_photo'), NOT averaged.\n"
        "  New residents get home_floor=0 (\"no floor registered\"); set the real\n"
        "  floor later (e.g. via sqlite3 or the face_set_floor helper).\n",
        prog);
}

int main(int argc, char** argv) {
    std::string dir_path, db_path, schema_path = "db/schema.sql";
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
        else if (a == "--db")          db_path     = next("--db");
        else if (a == "--schema")      schema_path = next("--schema");
        else if (a == "--det-model")   det_model   = next("--det-model");
        else if (a == "--recog-model") recog_model = next("--recog-model");
        else if (a == "--recog-dim")   recog_dim   = std::atoi(next("--recog-dim"));
        else if (a == "--recog-bgr")   recog_rgb   = false;
        else if (a == "--min-face-px") min_face_px = std::atoi(next("--min-face-px"));
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    }
    if (dir_path.empty() || db_path.empty() ||
        det_model.empty() || recog_model.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    if (!fs::is_directory(dir_path)) {
        fprintf(stderr, "not a directory: %s\n", dir_path.c_str());
        return 1;
    }

    printf("[enroll] dir=%s  db=%s\n", dir_path.c_str(), db_path.c_str());
    printf("[enroll] det=%s (input=%dx%d, scrfd)\n",
           det_model.c_str(), NPU_INPUT_W, NPU_INPUT_H);
    printf("[enroll] recog=%s (dim=%d rgb=%d)\n",
           recog_model.c_str(), recog_dim, recog_rgb ? 1 : 0);

    // ---- Open the SQLite resident DB (create+schema if empty) ----------
    ResidentDB db;
    if (!db.open(db_path, schema_path)) {
        fprintf(stderr, "[enroll] failed to open resident DB %s\n", db_path.c_str());
        return 2;
    }

    awnn_init();
    // Load recog BEFORE detect (see main.cpp comment).
    FaceRecognizer* recognizer = new FaceRecognizer(recog_model, recog_dim, recog_rgb);
    if (!recognizer->model_loaded()) {
        fprintf(stderr, "[enroll] failed to load recog model\n");
        delete recognizer;
        awnn_uninit();
        return 2;
    }

    Awnn_Context_t* det_ctx = awnn_create(det_model.c_str());
    if (!det_ctx) {
        fprintf(stderr, "[enroll] failed to load detection model\n");
        delete recognizer;
        awnn_uninit();
        return 2;
    }

    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        fprintf(stderr, "[enroll] scrfd init failed\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 2;
    }

    std::vector<uint8_t> input_buf(NPU_INPUT_W * NPU_INPUT_H * 3);

    int total_persons = 0, total_imgs = 0, total_used = 0;

    for (const auto& person_entry : fs::directory_iterator(dir_path)) {
        if (!person_entry.is_directory()) continue;
        std::string person = person_entry.path().filename().string();

        // Create (or reuse) the resident row up front so we can attach each
        // image's embedding to it. home_floor=0 = "no floor registered yet".
        int64_t resident_id = db.upsert_resident(person, HOME_FLOOR_UNSET);
        if (resident_id < 0) {
            fprintf(stderr, "  [%s] upsert_resident failed (skip)\n", person.c_str());
            continue;
        }

        int person_embs = 0;
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

            Detection best;
            if (!detect_largest_face(det_ctx, scrfd, img, input_buf, best)) {
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

            // One embedding row per image — NO averaging (spec R6.1).
            if (!db.add_embedding(resident_id, "id_photo", e)) {
                fprintf(stderr, "  [%s] add_embedding failed for %s\n",
                        person.c_str(),
                        img_entry.path().filename().string().c_str());
                continue;
            }
            ++person_embs;
            ++total_used;
        }

        if (person_embs > 0) {
            ++total_persons;
            printf("  + %s (id=%lld, %d embeddings, home_floor=0)\n",
                   person.c_str(), (long long)resident_id, person_embs);
        } else {
            printf("  - %s: NO usable embedding (resident row kept, empty)\n",
                   person.c_str());
        }
    }

    printf("[enroll] persons=%d, imgs_scanned=%d, imgs_used=%d\n",
           total_persons, total_imgs, total_used);
    if (total_used == 0) {
        fprintf(stderr, "[enroll] no embeddings written\n");
        db.close();
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return 3;
    }
    printf("[enroll] wrote %d embeddings across %d residents -> %s\n",
           total_used, total_persons, db_path.c_str());
    printf("[enroll] NOTE: all new residents have home_floor=0 "
           "(\"no floor registered\"). Set real floors before cabin operation.\n");

    db.close();
    delete recognizer;
    awnn_destroy(det_ctx);
    awnn_uninit();
    return 0;
}
