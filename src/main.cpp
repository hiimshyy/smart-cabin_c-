// Face recognition realtime demo on Orange Pi A733 NPU.
//
// Two modes:
//  1. SCRFD-only (no --person-model):
//     capture -> SCRFD detect -> align -> recog -> match DB -> draw
//     Simple, stateless, ~25 FPS on USB cam.
//
//  2. YOLO + SCRFD + Tracker (--person-model provided):
//     capture -> YOLO person -> Tracker (IoU) -> SCRFD face
//              -> face-to-track associate -> recog scheduled -> draw
//     Robust identity persistence for elevator-style scenes where people
//     turn their back to the camera. ~15 FPS with all 3 NPU models.

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
#include "yolo_post.h"
#include "tracker.h"
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

// -------- Capture thread (latest-frame slot) ----------
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
        "    --match-thr F         Cosine similarity threshold (default 0.35)\n"
        "    --person-model PATH   Enable YOLO person detection + tracker\n"
        "    --person-thr F        Person score threshold (default 0.5)\n"
        "    --person-every N      Only run YOLO every N frames (default 1)\n"
        "    --track-iou F         Min IoU for track association (default 0.3)\n"
        "    --track-max-miss N    Kill track after N missed frames (default 30)\n"
        "    --recog-retry N       Re-verify recognition every N frames (default 90)\n"
        "    --source URL          RTSP/HTTP/file source (GStreamer)\n"
        "    --gst-pipeline STR    Custom GStreamer pipeline\n"
        "    --gst-latency MS      RTSP jitter buffer latency (default 100ms)\n"
        "    --windowed / --fullscreen\n",
        prog);
}

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
    const char* det_model_path    = "model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb";
    const char* recog_model_path  = nullptr;
    const char* face_db_path      = nullptr;
    const char* person_model_path = nullptr;
    const char* source_url        = nullptr;
    const char* custom_pipeline   = nullptr;
    int   cam_id           = 0;
    int   max_frames       = 0;
    int   recog_dim        = 512;
    int   gst_latency_ms   = 100;
    int   person_every     = 1;
    int   track_max_miss   = 30;
    int   recog_retry      = 90;
    float track_iou        = 0.3f;
    float person_thr       = 0.5f;
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
        auto sv = [&](const char* k) { return std::strcmp(a, k) == 0 && i + 1 < argc; };
        if      (sv("--frames"))         max_frames = std::atoi(argv[++i]);
        else if (sv("--recog-model"))    recog_model_path = argv[++i];
        else if (sv("--recog-dim"))      recog_dim = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--recog-bgr") == 0) recog_rgb = false;
        else if (sv("--face-db"))        face_db_path = argv[++i];
        else if (sv("--match-thr"))      match_threshold = (float)std::atof(argv[++i]);
        else if (sv("--person-model"))   person_model_path = argv[++i];
        else if (sv("--person-thr"))     person_thr = (float)std::atof(argv[++i]);
        else if (sv("--person-every"))   person_every = std::max(1, std::atoi(argv[++i]));
        else if (sv("--track-iou"))      track_iou = (float)std::atof(argv[++i]);
        else if (sv("--track-max-miss")) track_max_miss = std::atoi(argv[++i]);
        else if (sv("--recog-retry"))    recog_retry = std::atoi(argv[++i]);
        else if (sv("--source"))         source_url = argv[++i];
        else if (sv("--gst-pipeline"))   custom_pipeline = argv[++i];
        else if (sv("--gst-latency"))    gst_latency_ms = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--windowed") == 0)   fullscreen = false;
        else if (std::strcmp(a, "--fullscreen") == 0) fullscreen = true;
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    const bool use_tracker = (person_model_path != nullptr);

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

    FrameSlot   slot;
    std::thread cap_th(capture_worker, &cap, &slot);
    printf("[cam] capture thread started\n");

    // ---- 2) Init NPU. Load recog -> scrfd -> yolo (person detect last)
    //         This order minimizes NBG resource conflict on some models.
    awnn_init();

    auto shutdown_capture = [&]() {
        {
            std::lock_guard<std::mutex> lk(slot.mtx);
            slot.stop = true;
        }
        slot.cv_new.notify_all();
        if (cap_th.joinable()) cap_th.join();
        cap.release();
    };

    FaceRecognizer* recognizer = nullptr;
    FaceDB          face_db;
    bool            recog_enabled = false;
    if (recog_model_path) {
        recognizer = new FaceRecognizer(recog_model_path, recog_dim, recog_rgb);
        if (!recognizer->model_loaded()) {
            fprintf(stderr, "[recog] failed to load %s\n", recog_model_path);
            delete recognizer;
            awnn_uninit();
            shutdown_capture();
            return 2;
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

    Awnn_Context_t* det_ctx = awnn_create(det_model_path);
    if (!det_ctx) {
        fprintf(stderr, "awnn_create failed for face detection model %s\n", det_model_path);
        delete recognizer;
        awnn_uninit();
        shutdown_capture();
        return 2;
    }
    printf("[detect] loaded %s (input=%dx%d)\n", det_model_path, NPU_INPUT_W, NPU_INPUT_H);
    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        fprintf(stderr, "[scrfd] init failed\n");
        delete recognizer;
        awnn_destroy(det_ctx);
        awnn_uninit();
        shutdown_capture();
        return 3;
    }

    Awnn_Context_t* yolo_ctx = nullptr;
    YoloDecoder     yolo;
    Tracker         tracker;
    if (use_tracker) {
        yolo_ctx = awnn_create(person_model_path);
        if (!yolo_ctx) {
            fprintf(stderr, "[yolo] awnn_create failed for %s\n", person_model_path);
            delete recognizer;
            awnn_destroy(det_ctx);
            awnn_uninit();
            shutdown_capture();
            return 4;
        }
        printf("[yolo] loaded %s\n", person_model_path);
        if (!yolo.init(yolo_ctx, NPU_INPUT_W)) {
            fprintf(stderr, "[yolo] init failed\n");
            delete recognizer;
            awnn_destroy(yolo_ctx);
            awnn_destroy(det_ctx);
            awnn_uninit();
            shutdown_capture();
            return 4;
        }
        tracker.iou_thresh         = track_iou;
        tracker.max_missed         = track_max_miss;
        tracker.recog_retry_frames = recog_retry;
        printf("[track] enabled: iou=%.2f max_miss=%d recog_retry=%d person_every=%d\n",
               track_iou, track_max_miss, recog_retry, person_every);
    } else {
        printf("[track] disabled (no --person-model). Running SCRFD-only pipeline.\n");
    }

    // ---- 3) Buffers + display --------------------------------------------
    std::vector<uint8_t> npu_input(NPU_INPUT_W * NPU_INPUT_H * 3);

    DisplaySlot   disp_slot;
    std::thread   disp_th(display_worker, "Face Recog A733", &disp_slot, &g_stop, &slot, fullscreen);
    printf("[cam] display thread started\n");

    std::vector<Detection> faces;
    std::vector<PersonDet> persons;
    std::vector<float>     emb;
    faces.reserve(16);
    persons.reserve(16);
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
    StageStat s_cap, s_pre, s_yolo, s_scrfd, s_track, s_recog, s_draw, s_e2e;
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
        if (frame.empty()) continue;
        double t1 = now_ms();

        // ---- Preprocess (letterbox 640x640, shared for YOLO + SCRFD) ----
        PreInfo pre;
        detect_preprocess(frame, npu_input.data(),
                          NPU_INPUT_W, NPU_INPUT_H, pre);
        double t2 = now_ms();

        // ---- YOLO person detect (optional, skipped some frames) ----
        double t_yolo = 0.0;
        if (use_tracker && (frame_id % person_every == 0)) {
            void* ins[] = { npu_input.data() };
            double y0 = now_ms();
            awnn_set_input_buffers(yolo_ctx, ins);
            awnn_run(yolo_ctx);
            float** youts = awnn_get_output_buffers(yolo_ctx);
            yolo.decode(youts, pre, person_thr, /*nms=*/0.45f, persons);
            t_yolo = now_ms() - y0;
            tracker.update(persons, frame_id);
        }
        // On skipped frames, tracker still ages — advance with empty detections?
        // No: we shouldn't kill tracks just because we skipped YOLO. Skip update.
        double t3 = now_ms();

        // ---- SCRFD face detect ----
        void* det_inputs[] = { npu_input.data() };
        awnn_set_input_buffers(det_ctx, det_inputs);
        awnn_run(det_ctx);
        float** dets_out = awnn_get_output_buffers(det_ctx);
        scrfd.decode(dets_out, pre, /*score=*/0.5f, /*nms=*/0.4f, faces);
        double t4 = now_ms();

        // ---- Face → Track association + recognition scheduling ----
        // For each face:
        //   - if tracker on: find enclosing person track; else use face bbox directly
        //   - if track needs_recog (or no tracker): run recog, record result
        struct FaceLabel {
            std::string name;
            float       sim = -1.0f;
            char        stat = '.';
            int         track_id = -1;
        };
        std::vector<FaceLabel> face_labels(faces.size());

        double t_recog_total = 0.0;
        if (recog_enabled && recognizer) {
            double r0 = now_ms();
            for (size_t f = 0; f < faces.size(); ++f) {
                const auto& fd = faces[f];
                float cx = 0.5f * (fd.x1 + fd.x2);
                float cy = 0.5f * (fd.y1 + fd.y2);

                Track* linked_track = nullptr;
                bool   need_recog   = true;
                if (use_tracker) {
                    linked_track = tracker.find_track_for_face(cx, cy);
                    if (linked_track) {
                        face_labels[f].track_id = linked_track->id;
                        need_recog = tracker.needs_recog(*linked_track, frame_id);
                        // Even if no recog this frame, propagate cached name to label
                        face_labels[f].name = linked_track->name;
                        face_labels[f].sim  = linked_track->match_sim;
                        face_labels[f].stat = need_recog ? 'r' : 'c';   // r=recog now, c=cached
                    } else {
                        // Face detected but no person track around it — recog anyway
                        // (person detection may have missed this person for a frame)
                        face_labels[f].stat = 'o';   // orphan
                    }
                }

                if (!need_recog) continue;

                align_face_112(frame, fd.landmarks, aligned);
                if (aligned.empty()) { face_labels[f].stat = 'A'; continue; }
                if (!recognizer->extract(aligned, emb)) { face_labels[f].stat = 'E'; continue; }
                if (face_db.size() == 0) { face_labels[f].stat = 'D'; continue; }

                float sim = 0.0f;
                int idx = face_db.match(emb, match_threshold, sim);
                std::string name;
                char stat;
                if (idx >= 0) { name = face_db.all()[idx].name; stat = 'M'; }
                else          { name = "unknown";               stat = 'U'; }

                face_labels[f].name = name;
                face_labels[f].sim  = sim;
                face_labels[f].stat = stat;

                if (linked_track) {
                    tracker.record_recognition(linked_track->id, name, sim, frame_id);
                    // linked_track pointer may have been invalidated by identity
                    // inheritance (which reorders IDs), so re-lookup for logging.
                    face_labels[f].track_id = linked_track->id;
                }
            }
            t_recog_total = now_ms() - r0;

            static int log_counter = 0;
            if (++log_counter % 15 == 0 && !faces.empty()) {
                fprintf(stdout, "[recog] frame %d:", frame_id);
                for (size_t f = 0; f < faces.size(); ++f) {
                    const auto& L = face_labels[f];
                    fprintf(stdout, " f%zu={%s,%.2f,%c,id=%d}",
                            f, L.name.empty() ? "-" : L.name.c_str(),
                            L.sim, L.stat, L.track_id);
                }
                fprintf(stdout, "\n"); fflush(stdout);
            }
        }
        double t5 = now_ms();

        // ---- Draw overlay ----
        // Prefer drawing tracks (if enabled) over raw face bboxes for stability.
        if (use_tracker) {
            for (Track* t : tracker.active_tracks()) {
                bool known    = !t->name.empty() && t->name != "unknown";
                bool unknown_tried = t->name == "unknown";
                cv::Scalar color = known ? cv::Scalar(0, 255, 0)     // green
                                 : unknown_tried ? cv::Scalar(0, 0, 255)   // red
                                 : cv::Scalar(200, 200, 0);          // yellow (untried)

                cv::rectangle(frame,
                              cv::Point(cvRound(t->x1), cvRound(t->y1)),
                              cv::Point(cvRound(t->x2), cvRound(t->y2)),
                              color, 2);
                char lbl[128];
                if (known) {
                    std::snprintf(lbl, sizeof(lbl), "#%d %s %.2f",
                                  t->id, t->name.c_str(), t->match_sim);
                } else if (unknown_tried) {
                    std::snprintf(lbl, sizeof(lbl), "#%d unknown", t->id);
                } else {
                    std::snprintf(lbl, sizeof(lbl), "#%d ...", t->id);
                }
                cv::putText(frame, lbl,
                            cv::Point(cvRound(t->x1), cvRound(t->y1) - 6),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
            }
            // Draw face bbox (small, thin) to visualize face detection quality
            for (const auto& fd : faces) {
                cv::rectangle(frame,
                              cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                              cv::Point(cvRound(fd.x2), cvRound(fd.y2)),
                              cv::Scalar(255, 255, 0), 1);   // cyan, thin
                for (int k = 0; k < 5; ++k) {
                    cv::circle(frame,
                               cv::Point(cvRound(fd.landmarks[k*2]),
                                         cvRound(fd.landmarks[k*2+1])),
                               1, cv::Scalar(255, 255, 0), -1);
                }
            }
        } else {
            // SCRFD-only fallback: draw face bboxes with name
            for (size_t f = 0; f < faces.size(); ++f) {
                const auto& fd = faces[f];
                const auto& L  = face_labels[f];
                bool is_unknown = recog_enabled && L.name == "unknown";
                cv::Scalar color = is_unknown
                    ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
                cv::rectangle(frame,
                              cv::Point(cvRound(fd.x1), cvRound(fd.y1)),
                              cv::Point(cvRound(fd.x2), cvRound(fd.y2)),
                              color, 2);
                char lbl[128];
                if (recog_enabled && !L.name.empty()) {
                    std::snprintf(lbl, sizeof(lbl), "%s %.2f", L.name.c_str(), L.sim);
                } else {
                    std::snprintf(lbl, sizeof(lbl), "%.0f%%", fd.score * 100.0f);
                }
                cv::putText(frame, lbl,
                            cv::Point(cvRound(fd.x1), cvRound(fd.y1) - 6),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
                static const cv::Scalar lm_colors[5] = {
                    {0,0,255}, {0,255,255}, {255,0,255}, {0,255,0}, {255,0,0}
                };
                for (int k = 0; k < 5; ++k) {
                    cv::circle(frame,
                               cv::Point(cvRound(fd.landmarks[k*2]),
                                         cvRound(fd.landmarks[k*2+1])),
                               2, lm_colors[k], -1);
                }
            }
        }

        ++frames_since_report;
        double now = now_ms();
        if (now - last_report > 500.0) {
            fps = frames_since_report * 1000.0 / (now - last_report);
            last_report = now;
            frames_since_report = 0;
            if (use_tracker) {
                printf("[frame %d] fps=%.1f cap=%.1f pre=%.1f yolo=%.1f scrfd=%.1f "
                       "recog=%.1f faces=%zu tracks=%d\n",
                       frame_id, fps, t1-t0, t2-t1, t_yolo, t4-t3,
                       t_recog_total, faces.size(), tracker.active_count());
            } else {
                printf("[frame %d] fps=%.1f cap=%.1f pre=%.1f npu=%.1f post=%.1f "
                       "recog=%.1f faces=%zu\n",
                       frame_id, fps, t1-t0, t2-t1, t4-t3, 0.0,
                       t_recog_total, faces.size());
            }
            fflush(stdout);
        }
        char hud[192];
        if (use_tracker) {
            std::snprintf(hud, sizeof(hud),
                "FPS %.1f | cap %.1f pre %.1f yolo %.1f scrfd %.1f recog %.1f | tracks %d",
                fps, t1-t0, t2-t1, t_yolo, t4-t3, t_recog_total,
                tracker.active_count());
        } else {
            std::snprintf(hud, sizeof(hud),
                "FPS %.1f | cap %.1f pre %.1f npu %.1f recog %.1f | faces %zu",
                fps, t1-t0, t2-t1, t4-t3, t_recog_total, faces.size());
        }
        {
            cv::Rect bg_rect(0, 0, frame.cols, 32);
            cv::Mat  bg_roi   = frame(bg_rect);
            cv::Mat  bg_layer(bg_roi.size(), bg_roi.type(),
                              cv::Scalar(40, 40, 40));
            cv::addWeighted(bg_layer, 0.55, bg_roi, 0.45, 0, bg_roi);
        }
        cv::putText(frame, hud, cv::Point(6, 20),
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
            if (use_tracker && t_yolo > 0) s_yolo.add(t_yolo);
            s_scrfd.add(t4 - t3);
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
    if (use_tracker && s_yolo.n > 0) {
        printf("yolo (%dx)  %8.2f  %8.2f  %8.2f\n",
               person_every, s_yolo.avg(), s_yolo.mn, s_yolo.mx);
    }
    printf("scrfd      %8.2f  %8.2f  %8.2f\n", s_scrfd.avg(), s_scrfd.mn, s_scrfd.mx);
    printf("recog+match%8.2f  %8.2f  %8.2f\n", s_recog.avg(), s_recog.mn, s_recog.mx);
    printf("draw+show  %8.2f  %8.2f  %8.2f\n", s_draw.avg(),  s_draw.mn,  s_draw.mx);
    printf("end-to-end %8.2f  %8.2f  %8.2f\n", s_e2e.avg(),   s_e2e.mn,   s_e2e.mx);
    printf("=============================\n");

    printf("[shutdown] stopping capture thread...\n");
    shutdown_capture();

    printf("[shutdown] stopping display thread...\n");
    {
        std::lock_guard<std::mutex> lk(disp_slot.mtx);
        disp_slot.stop = true;
    }
    disp_slot.cv_new.notify_all();
    if (disp_th.joinable()) disp_th.join();

    printf("[shutdown] destroying NPU...\n");
    delete recognizer;
    if (yolo_ctx) awnn_destroy(yolo_ctx);
    awnn_destroy(det_ctx);
    awnn_uninit();
    return 0;
}
