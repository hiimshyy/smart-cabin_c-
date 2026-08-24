// capture_person: interactive tool to capture N face frames of ONE person
// from the camera, saving into <out>/<name>/frame_NN.jpg.
// Uses RetinaFace detection to gate captures (auto-shoot only when exactly
// one clean face is present and bbox is large enough).
//
// Usage:
//   ./capture_person --name alice [--out faces] [--count 5]
//                    [--cam 0] [--min-face-px 100]
//                    [--det-model model/Retinaface_resnet50_320_uint8_a733.nb]
//
// Controls (in preview window):
//   SPACE : manual capture (bypass auto-gate)
//   q/ESC : quit early

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <csignal>
#include <atomic>
#include <filesystem>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

#include <awnn_lib.h>

#include "anchors.h"
#include "retinaface_pre.h"
#include "retinaface_post.h"

namespace fs = std::filesystem;

static constexpr int NPU_INPUT_W = 320;
static constexpr int NPU_INPUT_H = 320;
static constexpr int CAM_W       = 640;
static constexpr int CAM_H       = 480;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

static void print_usage(const char* p) {
    fprintf(stderr,
        "Usage: %s --name <name> [options]\n"
        "  --name X          Identity/folder name (required)\n"
        "  --out DIR         Root folder (default: faces)\n"
        "  --count N         Frames to capture (default: 5)\n"
        "  --cam N           /dev/videoN (default: 0)\n"
        "  --min-face-px N   Skip when bbox width/height < N (default: 100)\n"
        "  --det-model PATH  RetinaFace .nb (default: model/Retinaface_resnet50_320_uint8_a733.nb)\n"
        "  --interval-ms N   Min ms between auto-captures (default: 500)\n"
        "  --no-preview      Headless mode (no window, purely auto)\n",
        p);
}

int main(int argc, char** argv) {
    std::string name;
    std::string out_root  = "faces";
    std::string det_model = "model/Retinaface_resnet50_320_uint8_a733.nb";
    int  count            = 5;
    int  cam_id           = 0;
    int  min_face_px      = 100;
    int  interval_ms      = 500;
    bool preview          = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* w) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", w); exit(2); }
            return argv[++i];
        };
        if      (a == "--name")         name        = need("--name");
        else if (a == "--out")          out_root    = need("--out");
        else if (a == "--count")        count       = std::atoi(need("--count"));
        else if (a == "--cam")          cam_id      = std::atoi(need("--cam"));
        else if (a == "--min-face-px")  min_face_px = std::atoi(need("--min-face-px"));
        else if (a == "--det-model")    det_model   = need("--det-model");
        else if (a == "--interval-ms")  interval_ms = std::atoi(need("--interval-ms"));
        else if (a == "--no-preview")   preview     = false;
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    }
    if (name.empty()) { print_usage(argv[0]); return 1; }

    fs::path save_dir = fs::path(out_root) / name;
    fs::create_directories(save_dir);
    printf("[capture] name=%s out=%s count=%d min_face=%d\n",
           name.c_str(), save_dir.string().c_str(), count, min_face_px);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- Open camera --------------------------------------------------
    cv::VideoCapture cap;
    cap.open(cam_id, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        fprintf(stderr, "Cannot open /dev/video%d\n", cam_id);
        return 3;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  CAM_W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, CAM_H);
    cap.set(cv::CAP_PROP_FPS, 30);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // ---- Load detection model ----------------------------------------
    awnn_init();
    Awnn_Context_t* det_ctx = awnn_create(det_model.c_str());
    if (!det_ctx) {
        fprintf(stderr, "awnn_create failed for %s\n", det_model.c_str());
        awnn_uninit();
        return 4;
    }
    printf("[capture] detection model loaded\n");

    auto anchors = generate_retinaface_anchors(NPU_INPUT_W);
    std::vector<uint8_t> npu_input(NPU_INPUT_W * NPU_INPUT_H * 3);
    std::vector<Detection> dets;

    if (preview) {
        cv::namedWindow("capture_person", cv::WINDOW_AUTOSIZE);
    }

    int  saved = 0;
    double last_save_ms = 0.0;
    cv::Mat frame, clean_frame;

    while (saved < count && !g_stop.load()) {
        if (!cap.read(frame) || frame.empty()) {
            fprintf(stderr, "grab failed\n");
            break;
        }
        // Keep a pristine copy for saving (overlay drawing is destructive)
        clean_frame = frame.clone();

        // Detect
        PreInfo pre;
        retinaface_preprocess(frame, npu_input.data(),
                              NPU_INPUT_W, NPU_INPUT_H, pre);
        void* ins[] = { npu_input.data() };
        awnn_set_input_buffers(det_ctx, ins);
        awnn_run(det_ctx);
        float** outs = awnn_get_output_buffers(det_ctx);
        retinaface_postprocess(outs[0], outs[1], outs[2],
                               anchors, pre, 0.5f, 0.4f, dets);

        // Decide capture-worthiness: exactly one face, big enough
        std::string status;
        bool auto_ok = false;
        if (dets.empty()) {
            status = "no face — align to camera";
        } else if (dets.size() > 1) {
            status = "multiple faces — only 1 person please";
        } else {
            const auto& d = dets.front();
            float fw = d.x2 - d.x1, fh = d.y2 - d.y1;
            if (fw < min_face_px || fh < min_face_px) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "too small (%.0fx%.0f, need >=%d) — move closer",
                              fw, fh, min_face_px);
                status = buf;
            } else if (d.score < 0.9f) {
                status = "low confidence — improve lighting";
            } else {
                status = "OK — hold still";
                auto_ok = true;
            }
        }

        // Draw overlay
        for (const auto& d : dets) {
            cv::rectangle(frame,
                          cv::Point(cvRound(d.x1), cvRound(d.y1)),
                          cv::Point(cvRound(d.x2), cvRound(d.y2)),
                          auto_ok ? cv::Scalar(0,255,0) : cv::Scalar(0,200,255), 2);
            for (int k = 0; k < 5; ++k) {
                cv::circle(frame,
                           cv::Point(cvRound(d.landmarks[k*2]),
                                     cvRound(d.landmarks[k*2+1])),
                           2, cv::Scalar(0,255,0), -1);
            }
        }
        char hud[160];
        std::snprintf(hud, sizeof(hud),
                      "%s  |  captured %d/%d",
                      status.c_str(), saved, count);
        cv::putText(frame, hud, cv::Point(6, 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 0, 0), 4);
        cv::putText(frame, hud, cv::Point(6, 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, "SPACE=shoot   q=quit", cv::Point(6, 460),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 2);

        // Auto-capture gate
        double t = now_ms();
        bool do_save = false;
        int  key = -1;

        if (preview) {
            cv::imshow("capture_person", frame);
            key = cv::waitKey(1) & 0xFF;
        }
        if (auto_ok && (t - last_save_ms) >= interval_ms) do_save = true;
        if (key == ' ') do_save = true;     // manual override (with or without gate)
        if (key == 'q' || key == 27) break;

        if (do_save) {
            char fn[64];
            std::snprintf(fn, sizeof(fn), "frame_%02d.jpg", saved + 1);
            fs::path save_path = save_dir / fn;
            if (cv::imwrite(save_path.string(), clean_frame)) {
                ++saved;
                last_save_ms = t;
                printf("[capture] saved %s (%d/%d)\n",
                       save_path.string().c_str(), saved, count);
            }
        }
    }

    printf("[capture] done: %d/%d frames saved to %s\n",
           saved, count, save_dir.string().c_str());

    awnn_destroy(det_ctx);
    awnn_uninit();
    if (preview) cv::destroyAllWindows();
    cap.release();
    return (saved > 0) ? 0 : 5;
}
