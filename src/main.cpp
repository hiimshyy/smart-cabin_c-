// SCRFD + MobileFaceNet realtime demo on Orange Pi A733 NPU.
// Pipeline: USB cam / RTSP → letterbox 640 → NPU (SCRFD) → decode+NMS →
//           align 112 → NPU (MobileFaceNet) → cosine match vs .fdb DB.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include <awnn_lib.h>

#include "detect_pre.h"
#include "detection.h"
#include "scrfd_post.h"
#include "face_align.h"
#include "face_recog.h"
#include "face_db.h"

static constexpr int NPU_INPUT_W = 640;
static constexpr int NPU_INPUT_H = 640;
static constexpr int CAM_W       = 640;
static constexpr int CAM_H       = 480;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

// -------- Threading: latest-frame slot ----------
// Producer (capture thread) always overwrites the slot with the newest frame.
// Consumer (main) waits for a new sequence number, then copies it out.
// No queue → we never process stale frames.
struct FrameSlot {
    std::mutex               mtx;
    std::condition_variable  cv_new;
    cv::Mat                  latest;
    uint64_t                 seq  = 0;
    bool                     stop = false;
};

static void capture_worker(cv::VideoCapture* cap, FrameSlot* slot) {
    cv::Mat local;
    while (true) {
        if (!cap->read(local) || local.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            {
                std::lock_guard<std::mutex> lk(slot->mtx);
                if (slot->stop) return;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(slot->mtx);
            if (slot->stop) return;
            local.copyTo(slot->latest);
            slot->seq++;
        }
        slot->cv_new.notify_one();
    }
}

// -------- Display thread ----------
struct DisplaySlot {
    std::mutex               mtx;
    std::condition_variable  cv_new;
    cv::Mat                  latest;
    uint64_t                 seq  = 0;
    bool                     stop = false;
    int                      last_key = -1;
};

static void display_worker(const char* win_name, DisplaySlot* slot,
                           std::atomic<bool>* stop_flag,
                           FrameSlot* capture_slot,
                           bool fullscreen) {
    if (fullscreen) {
        cv::namedWindow(win_name, cv::WINDOW_NORMAL);
        cv::setWindowProperty(win_name, cv::WND_PROP_FULLSCREEN,
                              cv::WINDOW_FULLSCREEN);
    } else {
        cv::namedWindow(win_name, cv::WINDOW_AUTOSIZE);
    }
    cv::Mat local;
    uint64_t last_seq = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(slot->mtx);
            slot->cv_new.wait(lk, [&]{
                return slot->seq != last_seq || slot->stop;
            });
            if (slot->stop) break;
            slot->latest.copyTo(local);
            last_seq = slot->seq;
        }
        cv::imshow(win_name, local);
        int k = cv::waitKey(1) & 0xFF;
        if (k == 'q' || k == 27) {
            stop_flag->store(true);
            if (capture_slot) capture_slot->cv_new.notify_all();
            std::lock_guard<std::mutex> lk(slot->mtx);
            slot->last_key = k;
            break;
        }
    }
    cv::destroyWindow(win_name);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [detect.nb] [cam_id] [options]\n"
        "  positional args (optional):\n"
        "    detect.nb   SCRFD .nb path (default: model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb)\n"
        "    cam_id      /dev/videoN index (default: 0)\n"
        "  options:\n"
        "    --frames N            Run N frames then exit with benchmark summary\n"
        "    --recog-model PATH    Enable face recognition, path to recog .nb\n"
        "    --recog-dim N         Embedding dimension (default 512)\n"
        "    --recog-bgr           Feed BGR to recog (default RGB swap on)\n"
        "    --face-db PATH        Load .fdb identity database\n"
        "    --match-thr F         Cosine similarity threshold for match (default 0.35)\n"
        "    --source URL          RTSP/HTTP/file source (uses GStreamer). Overrides cam_id.\n"
        "    --gst-pipeline STR    Custom GStreamer pipeline (must end with `! appsink`).\n"
        "    --gst-latency MS      RTSP jitter buffer latency (default 100ms)\n"
        "    --windowed            Show output in a normal window (default: fullscreen)\n"
        "    --fullscreen          Force fullscreen (default on)\n",
        prog);
}

// Build a default GStreamer pipeline for RTSP/HTTP/file URL.
static std::string build_gst_pipeline(const std::string& url, int latency_ms) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "uridecodebin uri=%s buffer-duration=%d000000 ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=BGR ! "
        "appsink drop=true sync=false max-buffers=1",
        url.c_str(), latency_ms);
    return std::string(buf);
}

static bool is_stream_source(const std::string& s) {
    return s.rfind("rtsp://", 0) == 0 ||
           s.rfind("http://", 0) == 0 ||
           s.rfind("https://", 0) == 0 ||
           s.rfind("file://", 0) == 0 ||
           s.find(".mp4") != std::string::npos ||
           s.find(".mkv") != std::string::npos ||
           s.find(".avi") != std::string::npos;
}

int main(int argc, char** argv) {
    // ---- Parse CLI --------------------------------------------------------
    const char* det_model_path   = "model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb";
    const char* recog_model_path = nullptr;
    const char* face_db_path     = nullptr;
    const char* source_url       = nullptr;
    const char* custom_pipeline  = nullptr;
    int   cam_id           = 0;
    int   max_frames       = 0;
    int   recog_dim        = 512;
    int   gst_latency_ms   = 100;
    bool  recog_rgb        = true;
    bool  fullscreen       = true;
    float match_threshold  = 0.35f;

    int positional_idx = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' && positional_idx < 2) {
            if (positional_idx == 0) det_model_path = a;
            else if (positional_idx == 1) cam_id = std::atoi(a);
            ++positional_idx;
            continue;
        }
        if (std::strcmp(a, "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--recog-model") == 0 && i + 1 < argc) {
            recog_model_path = argv[++i];
        } else if (std::strcmp(a, "--recog-dim") == 0 && i + 1 < argc) {
            recog_dim = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--recog-bgr") == 0) {
            recog_rgb = false;
        } else if (std::strcmp(a, "--face-db") == 0 && i + 1 < argc) {
            face_db_path = argv[++i];
        } else if (std::strcmp(a, "--match-thr") == 0 && i + 1 < argc) {
            match_threshold = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--source") == 0 && i + 1 < argc) {
            source_url = argv[++i];
        } else if (std::strcmp(a, "--gst-pipeline") == 0 && i + 1 < argc) {
            custom_pipeline = argv[++i];
        } else if (std::strcmp(a, "--gst-latency") == 0 && i + 1 < argc) {
            gst_latency_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--windowed") == 0) {
            fullscreen = false;
        } else if (std::strcmp(a, "--fullscreen") == 0) {
            fullscreen = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- 1) Open camera ---------------------------------------------------
    cv::VideoCapture cap;
    bool is_stream = (custom_pipeline != nullptr) ||
                     (source_url != nullptr && is_stream_source(source_url));

    if (is_stream) {
        std::string pipeline = custom_pipeline
            ? std::string(custom_pipeline)
            : build_gst_pipeline(source_url, gst_latency_ms);
        printf("[cam] GStreamer pipeline:\n  %s\n", pipeline.c_str());
        cap.open(pipeline, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            fprintf(stderr, "Cannot open GStreamer stream.\n");
            return 1;
        }
    } else {
        cap.open(cam_id, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            fprintf(stderr, "Cannot open /dev/video%d\n", cam_id);
            return 1;
        }
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
        cap.set(cv::CAP_PROP_FRAME_WIDTH,  CAM_W);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, CAM_H);
        cap.set(cv::CAP_PROP_FPS, 30);
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    }
    printf("[cam] %dx%d @ %.1f FPS (%s)\n",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS),
           is_stream ? "GStreamer" : "V4L2 MJPG");

    // ---- 1b) Launch capture thread ---------------------------------------
    FrameSlot   slot;
    std::thread cap_th(capture_worker, &cap, &slot);
    printf("[cam] capture thread started\n");

    // ---- 2) Init NPU. Load recognition FIRST, then detection.
    // Ordering avoids some NBG resource-conflict edge cases seen with newer
    // Acuity-compiled detection models blocking a second network creation.
    awnn_init();

    // ---- 2a) Optionally load recognition model + DB (before detection) ---
    FaceRecognizer* recognizer = nullptr;
    FaceDB          face_db;
    bool            recog_enabled = false;

    auto stop_capture_and_exit = [&](int code) {
        {
            std::lock_guard<std::mutex> lk(slot.mtx);
            slot.stop = true;
        }
        slot.cv_new.notify_all();
        if (cap_th.joinable()) cap_th.join();
        cap.release();
        return code;
    };

    if (recog_model_path) {
        recognizer = new FaceRecognizer(recog_model_path, recog_dim, recog_rgb);
        if (!recognizer->model_loaded()) {
            fprintf(stderr, "[recog] failed to load %s — aborting\n", recog_model_path);
            delete recognizer;
            awnn_uninit();
            return stop_capture_and_exit(2);
        }
        printf("[recog] loaded %s (dim=%d, rgb=%d)\n",
               recog_model_path, recog_dim, recog_rgb ? 1 : 0);
        if (face_db_path && face_db.load(face_db_path)) {
            printf("[recog] loaded DB %s: %zu identities, dim=%d\n",
                   face_db_path, face_db.size(), face_db.dim());
        } else if (face_db_path) {
            printf("[recog] WARN: cannot load DB %s — matching disabled\n", face_db_path);
        } else {
            printf("[recog] no --face-db given — matching disabled\n");
        }
        recog_enabled = true;
    }

    // ---- 2b) Load detection model AFTER recognition ----------------------
    Awnn_Context_t* det_ctx = awnn_create(det_model_path);
    if (!det_ctx) {
        fprintf(stderr, "awnn_create failed for detection model %s\n", det_model_path);
        delete recognizer;
        awnn_uninit();
        return stop_capture_and_exit(2);
    }
    printf("[detect] loaded %s (input=%dx%d)\n",
           det_model_path, NPU_INPUT_W, NPU_INPUT_H);

    // ---- 3) Init SCRFD decoder ------------------------------------------
    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        fprintf(stderr, "[scrfd] init failed — aborting\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        return stop_capture_and_exit(3);
    }

    // ---- 4) Buffers + display --------------------------------------------
    std::vector<uint8_t> npu_input(NPU_INPUT_W * NPU_INPUT_H * 3);

    DisplaySlot   disp_slot;
    std::thread   disp_th(display_worker, "Face Recog A733", &disp_slot, &g_stop, &slot, fullscreen);
    printf("[cam] display thread started\n");

    std::vector<Detection> dets;
    std::vector<float>     emb;
    dets.reserve(16);
    emb.reserve(recog_dim);

    cv::Mat frame, aligned;
    double  fps = 0.0;
    int     frame_id = 0;
    double  last_report = now_ms();
    int     frames_since_report = 0;

    struct StageStat {
        double sum = 0.0, mn = 1e9, mx = 0.0;
        int    n   = 0;
        void   add(double v) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); ++n; }
        double avg() const { return n ? sum / n : 0.0; }
    };
    StageStat s_cap, s_pre, s_npu, s_post, s_recog, s_draw, s_e2e;
    double run_start = now_ms();
    uint64_t last_seq = 0;

    while (true) {
        double t0 = now_ms();
        {
            std::unique_lock<std::mutex> lk(slot.mtx);
            slot.cv_new.wait(lk, [&]{
                return slot.seq != last_seq || slot.stop || g_stop.load();
            });
            if (slot.stop || g_stop.load()) break;
            slot.latest.copyTo(frame);
            last_seq = slot.seq;
        }
        if (frame.empty()) {
            fprintf(stderr, "Empty frame\n");
            continue;
        }
        double t1 = now_ms();

        // ---- Preprocess (letterbox 640x640) ----
        PreInfo pre;
        detect_preprocess(frame, npu_input.data(),
                          NPU_INPUT_W, NPU_INPUT_H, pre);
        double t2 = now_ms();

        // ---- Detection NPU ----
        void* det_inputs[] = { npu_input.data() };
        awnn_set_input_buffers(det_ctx, det_inputs);
        awnn_run(det_ctx);
        float** dets_out = awnn_get_output_buffers(det_ctx);
        double t3 = now_ms();

        // ---- SCRFD decode + NMS ----
        scrfd.decode(dets_out, pre, /*score=*/0.5f, /*nms=*/0.4f, dets);
        double t4 = now_ms();

        // ---- Recognition (per face) ----
        std::vector<std::string> names(dets.size());
        std::vector<float>       sims(dets.size(), -1.0f);
        // 'A'=align empty, 'E'=extract fail, 'D'=no DB, 'M'=matched, 'U'=unknown, '.'=not tried
        std::vector<char>        stats(dets.size(), '.');

        double t_recog_total = 0.0;
        if (recog_enabled && recognizer) {
            for (size_t f = 0; f < dets.size(); ++f) {
                align_face_112(frame, dets[f].landmarks, aligned);
                if (aligned.empty()) { stats[f] = 'A'; continue; }
                if (!recognizer->extract(aligned, emb)) { stats[f] = 'E'; continue; }
                if (face_db.size() > 0) {
                    float sim = 0.0f;
                    int   idx = face_db.match(emb, match_threshold, sim);
                    sims[f] = sim;
                    if (idx >= 0) { names[f] = face_db.all()[idx].name; stats[f] = 'M'; }
                    else          { names[f] = "unknown";               stats[f] = 'U'; }
                } else {
                    stats[f] = 'D';
                }
            }
            t_recog_total = now_ms() - t4;

            static int log_counter = 0;
            if (++log_counter % 10 == 0 && !dets.empty()) {
                fprintf(stdout, "[recog] frame %d:", frame_id);
                for (size_t f = 0; f < dets.size(); ++f) {
                    fprintf(stdout, " f%zu={%s,%.3f,st=%c}",
                            f, names[f].empty() ? "-" : names[f].c_str(),
                            sims[f], stats[f]);
                }
                fprintf(stdout, "\n"); fflush(stdout);
            }
        }
        double t5 = now_ms();

        // ---- Draw overlay ----
        for (size_t f = 0; f < dets.size(); ++f) {
            const auto& d = dets[f];
            bool is_unknown = recog_enabled && names[f] == "unknown";
            cv::Scalar color = is_unknown
                ? cv::Scalar(0,   0, 255)   // red
                : cv::Scalar(0, 255,   0);  // green

            cv::rectangle(frame,
                          cv::Point(cvRound(d.x1), cvRound(d.y1)),
                          cv::Point(cvRound(d.x2), cvRound(d.y2)),
                          color, 2);
            char lbl[128];
            if (recog_enabled && !names[f].empty()) {
                std::snprintf(lbl, sizeof(lbl), "%s %.2f", names[f].c_str(), sims[f]);
            } else {
                std::snprintf(lbl, sizeof(lbl), "%.0f%%", d.score * 100.0f);
            }
            cv::putText(frame, lbl,
                        cv::Point(cvRound(d.x1), cvRound(d.y1) - 6),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
            static const cv::Scalar lm_colors[5] = {
                {0,0,255}, {0,255,255}, {255,0,255}, {0,255,0}, {255,0,0}
            };
            for (int k = 0; k < 5; ++k) {
                cv::circle(frame,
                           cv::Point(cvRound(d.landmarks[k*2]),
                                     cvRound(d.landmarks[k*2+1])),
                           2, lm_colors[k], -1);
            }
        }

        ++frames_since_report;
        double now = now_ms();
        if (now - last_report > 500.0) {
            fps = frames_since_report * 1000.0 / (now - last_report);
            last_report = now;
            frames_since_report = 0;
            printf("[frame %d] fps=%.1f cap=%.1f pre=%.1f npu=%.1f post=%.1f "
                   "recog=%.1f faces=%zu\n",
                   frame_id, fps, t1-t0, t2-t1, t3-t2, t4-t3,
                   t_recog_total, dets.size());
            fflush(stdout);
        }
        char hud[160];
        std::snprintf(hud, sizeof(hud),
                      "FPS %.1f | cap %.1f pre %.1f npu %.1f post %.1f recog %.1f",
                      fps, t1-t0, t2-t1, t3-t2, t4-t3, t_recog_total);
        char det_hud[64];
        std::snprintf(det_hud, sizeof(det_hud), "faces: %zu | db: %zu",
                      dets.size(), face_db.size());

        // Translucent HUD background
        {
            cv::Rect bg_rect(0, 0, frame.cols, 52);
            cv::Mat  bg_roi   = frame(bg_rect);
            cv::Mat  bg_layer(bg_roi.size(), bg_roi.type(),
                              cv::Scalar(40, 40, 40));
            cv::addWeighted(bg_layer, 0.55, bg_roi, 0.45, 0, bg_roi);
        }

        cv::putText(frame, hud, cv::Point(6, 20),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 1);
        cv::putText(frame, det_hud, cv::Point(6, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 1);

        {
            std::lock_guard<std::mutex> lk(disp_slot.mtx);
            frame.copyTo(disp_slot.latest);
            disp_slot.seq++;
        }
        disp_slot.cv_new.notify_one();
        double t6 = now_ms();

        if (frame_id >= 3) {
            s_cap.add  (t1 - t0);
            s_pre.add  (t2 - t1);
            s_npu.add  (t3 - t2);
            s_post.add (t4 - t3);
            s_recog.add(t_recog_total);
            s_draw.add (t6 - t5);
            s_e2e.add  (t6 - t0);
        }
        ++frame_id;
        if (g_stop.load()) break;
        if (max_frames > 0 && frame_id >= max_frames) break;
    }

    // ---- Summary ---------------------------------------------------------
    double run_secs = (now_ms() - run_start) / 1000.0;
    printf("\n===== BENCHMARK SUMMARY =====\n");
    printf("Duration    : %.2f s\n", run_secs);
    printf("Frames done : %d\n", frame_id);
    printf("Avg FPS     : %.2f\n", frame_id / (run_secs > 0 ? run_secs : 1));
    printf("Stage        avg (ms)   min       max\n");
    printf("capture    %8.2f  %8.2f  %8.2f\n", s_cap.avg(),   s_cap.mn,   s_cap.mx);
    printf("preprocess %8.2f  %8.2f  %8.2f\n", s_pre.avg(),   s_pre.mn,   s_pre.mx);
    printf("npu-detect %8.2f  %8.2f  %8.2f\n", s_npu.avg(),   s_npu.mn,   s_npu.mx);
    printf("postproc   %8.2f  %8.2f  %8.2f\n", s_post.avg(),  s_post.mn,  s_post.mx);
    printf("recog+match%8.2f  %8.2f  %8.2f\n", s_recog.avg(), s_recog.mn, s_recog.mx);
    printf("draw+show  %8.2f  %8.2f  %8.2f\n", s_draw.avg(),  s_draw.mn,  s_draw.mx);
    printf("end-to-end %8.2f  %8.2f  %8.2f\n", s_e2e.avg(),   s_e2e.mn,   s_e2e.mx);
    printf("=============================\n");

    printf("[shutdown] stopping capture thread...\n");
    {
        std::lock_guard<std::mutex> lk(slot.mtx);
        slot.stop = true;
    }
    slot.cv_new.notify_all();
    if (cap_th.joinable()) cap_th.join();
    cap.release();

    printf("[shutdown] stopping display thread...\n");
    {
        std::lock_guard<std::mutex> lk(disp_slot.mtx);
        disp_slot.stop = true;
    }
    disp_slot.cv_new.notify_all();
    if (disp_th.joinable()) disp_th.join();

    printf("[shutdown] destroying NPU...\n");
    delete recognizer;
    awnn_destroy(det_ctx);
    awnn_uninit();
    return 0;
}
