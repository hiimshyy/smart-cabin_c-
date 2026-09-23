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
//
// This file is model initialization + pipeline orchestration + shutdown.
// CLI parsing (app_config), threaded video capture/display (video_io),
// overlay/HUD drawing (overlay), and the benchmark summary (benchmark) live in
// their own modules (spec main-cpp-refactor).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <string>

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
#include "resident_db.h"
#include "match_engine.h"
#include "interaction.h"
#include "log/logger.h"

#include "app_config.h"
#include "video_io.h"
#include "overlay.h"
#include "benchmark.h"
#include "face_label.h"

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

int main(int argc, char** argv) {
    // ---- Parse CLI --------------------------------------------------------
    AppConfig cfg;
    ParseResult pr = parse_args(argc, argv, cfg);
    if (pr.help_requested) return 0;

    // ---- Init application logger (system-logging spec) --------------------
    // Main app defaults: write a daily-rotated file under /var/log/face-cabin
    // (falls back to ./logs if that isn't writable). --log-level / --log-dir
    // and FACE_CABIN_LOG_* env override; CLI > env > default.
    {
        LogConfig def;
        def.to_file = true;
        def.dir     = "/var/log/face-cabin";
        Logger::instance().init(resolve_log_config(argc, argv, def));
    }
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    const bool use_tracker = (cfg.person_model_path != nullptr);

    // ---- 1) Open camera ---------------------------------------------------
    cv::VideoCapture cap;
    bool is_stream = (cfg.custom_pipeline != nullptr) ||
                     (cfg.source_url != nullptr && is_stream_source(cfg.source_url));

    // Build a reusable camera config so the capture thread can reopen the
    // source on its own if it drops (RTSP reconnect).
    CamConfig cam_cfg;
    cam_cfg.is_stream      = is_stream;
    cam_cfg.cam_id         = cfg.cam_id;
    cam_cfg.cam_w          = CAM_W;
    cam_cfg.cam_h          = CAM_H;
    cam_cfg.cam_fps        = 30;
    cam_cfg.backoff_min_ms = cfg.reconnect_min_ms;
    cam_cfg.backoff_max_ms = cfg.reconnect_max_ms;
    if (is_stream) {
        cam_cfg.pipeline = cfg.custom_pipeline
            ? std::string(cfg.custom_pipeline)
            : build_gst_pipeline(cfg.source_url, cfg.gst_latency_ms);
        LOG_INFO("cam", "GStreamer pipeline: %s", cam_cfg.pipeline.c_str());
    }

    bool cam_ready = open_capture(cap, cam_cfg);
    if (!cam_ready) {
        if (is_stream) {
            // Stream not up yet (network/camera still booting). Don't die —
            // let the capture thread keep retrying with backoff so a cabin
            // started before its RTSP source recovers on its own.
            LOG_WARN("cam", "stream not available yet — capture thread will "
                     "keep reconnecting (backoff %d-%d ms)",
                     cfg.reconnect_min_ms, cfg.reconnect_max_ms);
        } else {
            LOG_ERROR("cam", "cannot open /dev/video%d", cfg.cam_id);
            return 1;
        }
    } else {
        LOG_INFO("cam", "%dx%d @ %.1f FPS (%s)",
                 (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
                 (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
                 cap.get(cv::CAP_PROP_FPS),
                 is_stream ? "GStreamer" : "V4L2 MJPG");
    }

    FrameSlot   slot;
    std::thread cap_th(capture_worker, &cap, &slot, cam_cfg);
    LOG_INFO("cam", "capture thread started (reconnect backoff %d-%d ms)",
             cfg.reconnect_min_ms, cfg.reconnect_max_ms);

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

    // Resident-DB (operational) mode plumbing. When resident_db_path is set,
    // matching goes through MatchEngine (multi-embedding) instead of the .fdb
    // FaceDB, interaction sessions are tracked, and match_events are logged.
    ResidentDB          resident_db;
    MatchEngine         match_engine;
    InteractionManager  interaction;
    std::map<int64_t, Resident> resident_by_id;   // id -> metadata for overlay
    // Maps a track_id to the resident_id last recognized on it. This is the
    // authoritative link for cached tracker frames — resolving by name would
    // be ambiguous when two residents share a name/greeting_name (P1-2).
    std::map<int, int64_t> track_resident_id;
    bool                use_resident_db = false;

    if (cfg.recog_model_path) {
        recognizer = new FaceRecognizer(cfg.recog_model_path, cfg.recog_dim, cfg.recog_rgb);
        if (!recognizer->model_loaded()) {
            LOG_ERROR("recog", "failed to load model %s", cfg.recog_model_path);
            delete recognizer;
            awnn_uninit();
            shutdown_capture();
            return 2;
        }
        LOG_INFO("recog", "loaded %s (dim=%d, rgb=%d)",
                 cfg.recog_model_path, cfg.recog_dim, cfg.recog_rgb ? 1 : 0);

        // Prefer the operational SQLite path when --resident-db is given.
        if (cfg.resident_db_path) {
            if (!resident_db.open(cfg.resident_db_path)) {
                LOG_ERROR("db", "failed to open resident DB %s", cfg.resident_db_path);
                delete recognizer;
                awnn_uninit();
                shutdown_capture();
                return 2;
            }
            std::vector<Resident>     residents;
            std::vector<EmbeddingRow> embeddings;
            if (!resident_db.load_active(residents, embeddings)) {
                LOG_ERROR("db", "load_active failed on %s", cfg.resident_db_path);
                resident_db.close();
                delete recognizer;
                awnn_uninit();
                shutdown_capture();
                return 2;
            }
            // Cross-check embedding dim vs --recog-dim (P1-3). Enrolling with a
            // different dim than the runtime model silently drops every vector
            // (MatchEngine skips size mismatches), turning everyone "unknown".
            // Detect it up front and fail loudly instead.
            if (!embeddings.empty()) {
                int db_dim = (int)embeddings.front().vector.size();
                if (db_dim != cfg.recog_dim) {
                    LOG_ERROR("db", "embedding dim mismatch: DB has %d-D vectors "
                              "but --recog-dim=%d. All matches would fail. "
                              "Re-run with --recog-dim %d or re-enroll.",
                              db_dim, cfg.recog_dim, db_dim);
                    resident_db.close();
                    delete recognizer;
                    awnn_uninit();
                    shutdown_capture();
                    return 2;
                }
            }
            match_engine.build(embeddings, cfg.recog_dim);
            if (!embeddings.empty() && match_engine.vector_count() == 0) {
                LOG_ERROR("db", "no embeddings loaded into matcher despite %zu "
                          "rows in DB — check embedding dim consistency",
                          embeddings.size());
            }
            for (const auto& r : residents) {
                resident_by_id[r.id] = r;
            }

            InteractionConfig icfg;
            icfg.confirm_streak   = cfg.confirm_streak;
            icfg.cooldown_ms      = cfg.cooldown_ms;
            icfg.unknown_after_ms = cfg.unknown_after_ms;
            interaction.set_config(icfg);

            use_resident_db = true;
            LOG_INFO("db", "loaded %zu residents, %zu embeddings (dim=%d)",
                     residents.size(), embeddings.size(), cfg.recog_dim);
            LOG_INFO("db", "cabin_id=%d confirm_streak=%d cooldown_ms=%.0f "
                     "unknown_after_ms=%.0f",
                     cfg.cabin_id, cfg.confirm_streak, cfg.cooldown_ms, cfg.unknown_after_ms);
            if (match_engine.vector_count() == 0) {
                LOG_WARN("db", "no embeddings loaded — everyone will be unknown");
            }
        } else if (cfg.face_db_path && face_db.load(cfg.face_db_path)) {
            LOG_INFO("recog", "loaded .fdb %s: %zu identities, dim=%d",
                     cfg.face_db_path, face_db.size(), face_db.dim());
            LOG_WARN("recog", ".fdb is TEST/DEV mode (no floor/language/audit); "
                     "use --resident-db for real cabin operation");
        } else if (cfg.face_db_path) {
            LOG_WARN("recog", "cannot load .fdb %s — matching disabled", cfg.face_db_path);
        } else {
            LOG_INFO("recog", "no --face-db / --resident-db given — matching disabled");
        }
        recog_enabled = true;
    }

    Awnn_Context_t* det_ctx = awnn_create(cfg.det_model_path);
    if (!det_ctx) {
        LOG_ERROR("npu", "awnn_create failed for face detection model %s", cfg.det_model_path);
        delete recognizer;
        awnn_uninit();
        shutdown_capture();
        return 2;
    }
    LOG_INFO("detect", "loaded %s (input=%dx%d)", cfg.det_model_path, NPU_INPUT_W, NPU_INPUT_H);
    ScrfdDecoder scrfd;
    if (!scrfd.init(det_ctx, NPU_INPUT_W)) {
        LOG_ERROR("detect", "scrfd init failed");
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
        yolo_ctx = awnn_create(cfg.person_model_path);
        if (!yolo_ctx) {
            LOG_ERROR("npu", "awnn_create failed for person model %s", cfg.person_model_path);
            delete recognizer;
            awnn_destroy(det_ctx);
            awnn_uninit();
            shutdown_capture();
            return 4;
        }
        LOG_INFO("yolo", "loaded %s", cfg.person_model_path);
        if (!yolo.init(yolo_ctx, NPU_INPUT_W)) {
            LOG_ERROR("yolo", "init failed");
            delete recognizer;
            awnn_destroy(yolo_ctx);
            awnn_destroy(det_ctx);
            awnn_uninit();
            shutdown_capture();
            return 4;
        }
        tracker.iou_thresh         = cfg.track_iou;
        tracker.max_missed         = cfg.track_max_miss;
        tracker.recog_retry_frames = cfg.recog_retry;
        LOG_INFO("track", "enabled: iou=%.2f max_miss=%d recog_retry=%d person_every=%d",
                 cfg.track_iou, cfg.track_max_miss, cfg.recog_retry, cfg.person_every);
    } else {
        LOG_INFO("track", "disabled (no --person-model). SCRFD-only pipeline.");
    }

    // ---- 3) Buffers + display --------------------------------------------
    std::vector<uint8_t> npu_input(NPU_INPUT_W * NPU_INPUT_H * 3);

    DisplaySlot   disp_slot;
    std::thread   disp_th(display_worker, "Face Recog A733", &disp_slot, &g_stop, &slot, cfg.fullscreen);
    LOG_INFO("cam", "display thread started");

    std::vector<Detection> faces;
    std::vector<PersonDet> persons;
    std::vector<float>     emb;
    faces.reserve(16);
    persons.reserve(16);
    emb.reserve(cfg.recog_dim);

    cv::Mat frame, aligned;
    double  fps = 0.0;
    int     frame_id = 0;
    double  last_report = now_ms();
    int     frames_since_report = 0;

    StageStat s_cap, s_pre, s_yolo, s_scrfd, s_track, s_recog, s_draw, s_e2e;
    double run_start = now_ms();
    uint64_t last_seq = 0;
    UiScale ui;                       // computed lazily on first frame
    bool    ui_ready = false;

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

        // Initialize UI scale once we know the frame size.
        if (!ui_ready) {
            ui = UiScale::compute(frame.rows, cfg.ui_scale_override);
            LOG_INFO("ui", "frame=%dx%d ui_scale=%.2f%s (font=%.2f/%.2f, "
                     "line=%d/%d, hud_h=%d)",
                     frame.cols, frame.rows, ui.scale,
                     cfg.ui_scale_override > 0 ? " (manual)" : " (auto)",
                     ui.font_label, ui.font_hud,
                     ui.line_thick, ui.line_thin, ui.hud_h);
            ui_ready = true;
        }

        // ---- Preprocess (letterbox 640x640, shared for YOLO + SCRFD) ----
        PreInfo pre;
        detect_preprocess(frame, npu_input.data(),
                          NPU_INPUT_W, NPU_INPUT_H, pre);
        double t2 = now_ms();

        // ---- YOLO person detect (optional, skipped some frames) ----
        double t_yolo = 0.0;
        if (use_tracker && (frame_id % cfg.person_every == 0)) {
            void* ins[] = { npu_input.data() };
            double y0 = now_ms();
            awnn_set_input_buffers(yolo_ctx, ins);
            awnn_run(yolo_ctx);
            float** youts = awnn_get_output_buffers(yolo_ctx);
            yolo.decode(youts, pre, cfg.person_thr, /*nms=*/0.45f, persons);
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
        std::vector<FaceLabel> face_labels(faces.size());

        // Collect (subject_key, MatchResult) for the interaction state machine.
        // subject_key = track_id when the tracker is on; otherwise we use a
        // single stable key (0) assigned to the largest face in the frame.
        std::vector<std::pair<int, MatchResult>> subjects;
        int    best_face_idx = -1;
        float  best_face_area = -1.0f;

        double t_recog_total = 0.0;
        if (recog_enabled && recognizer) {
            double r0 = now_ms();
            for (size_t f = 0; f < faces.size(); ++f) {
                const auto& fd = faces[f];
                float cx = 0.5f * (fd.x1 + fd.x2);
                float cy = 0.5f * (fd.y1 + fd.y2);
                float area = std::max(0.0f, (fd.x2 - fd.x1)) *
                             std::max(0.0f, (fd.y2 - fd.y1));
                face_labels[f].area = area;
                if (area > best_face_area) { best_face_area = area; best_face_idx = (int)f; }

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

                // ---- Matching: resident-db (MatchEngine) or legacy (.fdb) ----
                std::string name;
                char        stat;
                float       sim = 0.0f;
                int64_t     rid = -1;
                if (use_resident_db) {
                    if (match_engine.vector_count() == 0) {
                        face_labels[f].stat = 'D';
                        continue;
                    }
                    MatchResult mr = match_engine.match(emb, cfg.match_threshold);
                    sim = mr.similarity;
                    rid = mr.resident_id;
                    if (rid >= 0) {
                        auto it = resident_by_id.find(rid);
                        // Prefer greeting_name for the overlay; fall back to name.
                        if (it != resident_by_id.end() &&
                            !it->second.greeting_name.empty()) {
                            name = it->second.greeting_name;
                        } else if (it != resident_by_id.end()) {
                            name = it->second.name;
                        } else {
                            name = "id:" + std::to_string(rid);
                        }
                        stat = 'M';
                    } else {
                        name = "unknown";
                        stat = 'U';
                    }
                } else {
                    if (face_db.size() == 0) { face_labels[f].stat = 'D'; continue; }
                    int idx = face_db.match(emb, cfg.match_threshold, sim);
                    if (idx >= 0) { name = face_db.all()[idx].name; stat = 'M'; }
                    else          { name = "unknown";               stat = 'U'; }
                }

                face_labels[f].name        = name;
                face_labels[f].sim         = sim;
                face_labels[f].stat        = stat;
                face_labels[f].resident_id = rid;

                if (linked_track) {
                    tracker.record_recognition(linked_track->id, name, sim, frame_id);
                    // linked_track pointer may have been invalidated by identity
                    // inheritance (which reorders IDs), so re-lookup for logging.
                    face_labels[f].track_id = linked_track->id;
                    // Record the authoritative track_id -> resident_id link so
                    // cached frames (and the overlay) resolve the ID directly,
                    // never by ambiguous name (P1-2). Only on a real match.
                    if (use_resident_db && rid >= 0) {
                        track_resident_id[linked_track->id] = rid;
                    }
                }
            }
            t_recog_total = now_ms() - r0;

            static int log_counter = 0;
            if (++log_counter % 15 == 0 && !faces.empty() &&
                Logger::instance().enabled(LogLevel::DEBUG)) {
                // Per-frame detail is DEBUG-only (hot path). No PII (R7):
                // reference subjects by resident_id / track_id, never by name.
                std::string line;
                char seg[64];
                for (size_t f = 0; f < faces.size(); ++f) {
                    const auto& L = face_labels[f];
                    std::snprintf(seg, sizeof(seg), " f%zu={rid=%lld,%.2f,%c,tid=%d}",
                                  f, (long long)L.resident_id, L.sim, L.stat, L.track_id);
                    line += seg;
                }
                LOG_DEBUG("recog", "frame %d:%s", frame_id, line.c_str());
            }
        }

        // ---- Interaction state machine + match_events (resident-db mode) ----
        // Build one subject per visible face. subject_key:
        //   - tracker on : the face's track_id (stable across frames)
        //   - tracker off: the single largest face gets key 0 (others ignored,
        //                  a cabin only greets the person in front of it)
        // MatchResult resident_id/similarity come from face_labels; for cached
        // tracker frames (no recog this frame) we resolve the resident_id from
        // the track_id -> resident_id map recorded at the last real match (P1-2).
        if (use_resident_db) {
            subjects.clear();
            for (size_t f = 0; f < faces.size(); ++f) {
                const FaceLabel& L = face_labels[f];
                int subject_key;
                if (use_tracker) {
                    if (L.track_id < 0) continue;   // orphan face, no stable key
                    subject_key = L.track_id;
                } else {
                    if ((int)f != best_face_idx) continue;  // only the largest face
                    subject_key = 0;
                }

                MatchResult mr;
                mr.similarity  = L.sim;
                mr.resident_id = L.resident_id;
                // Cached tracker frame: label carries a cached name but no
                // resident_id. Resolve it from the track's authoritative id
                // link (never by name — avoids duplicate-name ambiguity).
                if (mr.resident_id < 0 && use_tracker && L.track_id >= 0) {
                    auto it = track_resident_id.find(L.track_id);
                    if (it != track_resident_id.end()) mr.resident_id = it->second;
                }
                subjects.emplace_back(subject_key, mr);
            }

            auto outcomes = interaction.update(subjects, now_ms());
            for (const auto& oc : outcomes) {
                MatchEvent ev;
                ev.cabin_id    = cfg.cabin_id;
                ev.similarity  = oc.similarity;
                ev.latency_ms  = (int)std::lround(t_recog_total);
                if (oc.confirmed) {
                    ev.resident_id    = oc.resident_id;
                    ev.action         = "matched";
                    // floor_selected left 0 (none): auto floor-call is Proposal 2.
                    ev.floor_selected = 0;
                    resident_db.log_event(ev);
                    resident_db.touch_resident(oc.resident_id);
                    auto it = resident_by_id.find(oc.resident_id);
                    int hf = (it != resident_by_id.end()) ? it->second.home_floor : 0;
                    // No PII (R7): log resident_id + floor only, never the name.
                    LOG_INFO("event", "CONFIRMED resident_id=%lld sim=%.2f home_floor=%d%s",
                             (long long)oc.resident_id, oc.similarity, hf,
                             hf <= HOME_FLOOR_UNSET ? " [no floor -> greet only]" : "");
                } else if (oc.unknown) {
                    ev.resident_id = -1;   // NULL
                    ev.action      = "unknown";
                    resident_db.log_event(ev);
                    LOG_INFO("event", "UNKNOWN subject_key=%d sim=%.2f",
                             oc.subject_key, oc.similarity);
                }
            }
        }
        double t5 = now_ms();

        // ---- Draw overlay ----
        // Prefer drawing tracks (if enabled) over raw face bboxes for stability.
        if (use_tracker) {
            draw_tracker_overlay(frame, tracker, faces, track_resident_id,
                                 resident_by_id, ui, use_resident_db);
        } else {
            draw_scrfd_overlay(frame, faces, face_labels, resident_by_id,
                               ui, recog_enabled, use_resident_db);
        }

        ++frames_since_report;
        double now = now_ms();
        if (now - last_report > 500.0) {
            fps = frames_since_report * 1000.0 / (now - last_report);
            last_report = now;
            frames_since_report = 0;
            if (use_tracker) {
                LOG_DEBUG("frame", "%d fps=%.1f cap=%.1f pre=%.1f yolo=%.1f scrfd=%.1f "
                          "recog=%.1f faces=%zu tracks=%d",
                          frame_id, fps, t1-t0, t2-t1, t_yolo, t4-t3,
                          t_recog_total, faces.size(), tracker.active_count());
            } else {
                LOG_DEBUG("frame", "%d fps=%.1f cap=%.1f pre=%.1f npu=%.1f post=%.1f "
                          "recog=%.1f faces=%zu",
                          frame_id, fps, t1-t0, t2-t1, t4-t3, 0.0,
                          t_recog_total, faces.size());
            }
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
        draw_hud(frame, hud, ui);

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
        if (cfg.max_frames > 0 && frame_id >= cfg.max_frames) break;
    }

    // ---- Summary (benchmark report — user-facing table) ------------------
    double run_secs = (now_ms() - run_start) / 1000.0;
    print_benchmark_summary(run_secs, frame_id, use_tracker, cfg.person_every,
                            s_cap, s_pre, s_yolo, s_scrfd, s_recog, s_draw, s_e2e);

    LOG_INFO("main", "stopping capture thread");
    shutdown_capture();

    LOG_INFO("main", "stopping display thread");
    {
        std::lock_guard<std::mutex> lk(disp_slot.mtx);
        disp_slot.stop = true;
    }
    disp_slot.cv_new.notify_all();
    if (disp_th.joinable()) disp_th.join();

    if (use_resident_db) {
        LOG_INFO("db", "flushing resident-db event writer");
        resident_db.close();
    }
    LOG_INFO("main", "destroying NPU contexts");
    delete recognizer;
    if (yolo_ctx) awnn_destroy(yolo_ctx);
    awnn_destroy(det_ctx);
    awnn_uninit();
    Logger::instance().shutdown();
    return 0;
}
