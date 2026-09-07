// add_person: add/update a single resident in the SQLite resident DB
// without re-enrolling the whole folder. Faster than rerunning enroll_faces.
//
// Each image contributes ONE embedding row (source='id_photo'); embeddings
// are never averaged (spec R6.2).
//   --merge   : append the new image embeddings to the resident.
//   --replace : delete the resident's existing embeddings, then add the new.
// A resident is created (home_floor=0) if the name does not exist yet.
//
// Usage:
//   ./add_person --name X --image img1.jpg [--image img2.jpg ...]
//                --db residents.db
//                --det-model <scrfd.nb>
//                --recog-model <recog.nb>
//                [--recog-dim N] [--recog-bgr]
//                [--replace | --merge]
//                [--min-face-px N (default 40)]
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
#include "log/logger.h"

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
        "           --db residents.db\n"
        "           --det-model <scrfd.nb>\n"
        "           --recog-model <recog.nb>\n"
        "           [--recog-dim N (default 512)]\n"
        "           [--recog-bgr    (feed BGR, default RGB)]\n"
        "           [--replace]     (delete resident's old embeddings first)\n"
        "           [--merge]       (append to resident's embeddings)\n"
        "           [--min-face-px N (default 40)]\n"
        "           [--schema PATH  (schema.sql for empty DB, default db/schema.sql)]\n"
        "\n"
        "  Each image -> one embedding row (source='id_photo'), NOT averaged.\n"
        "  New residents are created with home_floor=0 (\"no floor registered\").\n",
        prog);
}

int main(int argc, char** argv) {
    std::string name;
    std::vector<std::string> images;
    std::string db_path, schema_path = "db/schema.sql";
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
        else if (a == "--schema")       schema_path = next("--schema");
        else if (a == "--det-model")    det_model   = next("--det-model");
        else if (a == "--recog-model")  recog_model = next("--recog-model");
        else if (a == "--recog-dim")    recog_dim   = std::atoi(next("--recog-dim"));
        else if (a == "--recog-bgr")    recog_rgb   = false;
        else if (a == "--replace")      do_replace  = true;
        else if (a == "--merge")        do_merge    = true;
        else if (a == "--min-face-px")  min_face_px = std::atoi(next("--min-face-px"));
        else if (a == "--log-level" || a == "--log-dir") { next(a.c_str()); } // consumed by resolve_log_config
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

    // Logger: offline tool -> stderr only unless --log-dir given. No PII (R7):
    // never log the resident name; reference by resident_id.
    Logger::instance().init(resolve_log_config(argc, argv, LogConfig{}));
    LOG_INFO("add", "db=%s #images=%zu input=%dx%d",
             db_path.c_str(), images.size(), NPU_INPUT_W, NPU_INPUT_H);

    // ---- Open the SQLite resident DB (create+schema if empty) ----------
    ResidentDB db;
    if (!db.open(db_path, schema_path)) {
        LOG_ERROR("add", "cannot open resident DB %s", db_path.c_str());
        return 3;
    }

    // Does this resident already exist? Decide merge/replace semantics.
    int64_t existing_id = db.find_resident(name);
    if (existing_id >= 0 && !do_replace && !do_merge) {
        LOG_ERROR("add", "resident id=%lld already exists; use --replace to "
                  "overwrite its embeddings or --merge to add",
                  (long long)existing_id);
        db.close();
        return 4;
    }
    if (existing_id >= 0 && do_merge)
        LOG_INFO("add", "MERGE mode: appending embeddings to resident id=%lld",
                 (long long)existing_id);
    if (existing_id >= 0 && do_replace)
        LOG_INFO("add", "REPLACE mode: clearing old embeddings of resident id=%lld",
                 (long long)existing_id);

    awnn_init();
    // Load recog BEFORE detect.
    FaceRecognizer* recognizer = new FaceRecognizer(recog_model, recog_dim, recog_rgb);
    if (!recognizer->model_loaded()) {
        LOG_ERROR("add", "failed to load recog model");
        delete recognizer;
        awnn_uninit();
        db.close();
        return 5;
    }

    Awnn_Context_t* det_ctx = awnn_create(det_model.c_str());
    if (!det_ctx) {
        LOG_ERROR("add", "failed to load detection model");
        delete recognizer;
        awnn_uninit();
        db.close();
        return 5;
    }

    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        LOG_ERROR("add", "scrfd init failed");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        db.close();
        return 5;
    }

    std::vector<uint8_t> input_buf(NPU_INPUT_W * NPU_INPUT_H * 3);

    std::vector<std::vector<float>> new_embs;
    for (const auto& img_path : images) {
        // No PII (R7): the image path may embed the resident's name (e.g.
        // faces/<name>/frame_01.jpg), so log only the basename.
        std::string img_name = fs::path(img_path).filename().string();
        cv::Mat img = cv::imread(img_path, cv::IMREAD_COLOR);
        if (img.empty()) {
            LOG_WARN("add", "cannot read image %s (skip)", img_name.c_str());
            continue;
        }
        Detection best;
        if (!detect_largest_face(det_ctx, scrfd, img, input_buf, best)) {
            LOG_WARN("add", "no face detected in %s (skip)", img_name.c_str());
            continue;
        }
        float fw = best.x2 - best.x1, fh = best.y2 - best.y1;
        if (fw < min_face_px || fh < min_face_px) {
            LOG_WARN("add", "face too small %.0fx%.0f in %s (skip)",
                     fw, fh, img_name.c_str());
            continue;
        }

        cv::Mat aligned;
        align_face_112(img, best.landmarks, aligned);

        std::vector<float> e;
        if (!recognizer->extract(aligned, e)) {
            LOG_WARN("add", "embedding extract failed for %s", img_name.c_str());
            continue;
        }
        new_embs.push_back(std::move(e));
        LOG_INFO("add", "%s -> embedding OK", img_name.c_str());
    }

    if (new_embs.empty()) {
        LOG_ERROR("add", "no usable embedding extracted; aborting");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        db.close();
        return 6;
    }

    // ---- Write to SQLite (spec R6.2) -----------------------------------
    // Ensure the resident exists (creates with home_floor=0 if new).
    int64_t resident_id = db.upsert_resident(name, HOME_FLOOR_UNSET);
    if (resident_id < 0) {
        LOG_ERROR("add", "upsert_resident failed");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        db.close();
        return 7;
    }

    // --replace: drop the resident's old embeddings before adding new ones.
    if (do_replace) {
        if (!db.delete_embeddings(resident_id)) {
            LOG_ERROR("add", "delete_embeddings failed for id=%lld",
                      (long long)resident_id);
            delete recognizer;
            awnn_destroy(det_ctx);
            awnn_uninit();
            db.close();
            return 7;
        }
    }

    // Add one row per extracted embedding — NO averaging (spec R6.2).
    int written = 0;
    for (const auto& e : new_embs) {
        if (db.add_embedding(resident_id, "id_photo", e)) ++written;
        else LOG_WARN("add", "add_embedding failed (1 of %zu)", new_embs.size());
    }
    if (written == 0) {
        LOG_ERROR("add", "failed to write any embedding; aborting");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        db.close();
        return 7;
    }

    LOG_INFO("add", "resident id=%lld: wrote %d embedding(s)%s -> %s",
             (long long)resident_id, written,
             do_replace ? " (replaced old)" : (do_merge ? " (merged)" : ""),
             db_path.c_str());

    delete recognizer;
    awnn_destroy(det_ctx);
    awnn_uninit();
    db.close();
    Logger::instance().shutdown();
    return 0;
}
