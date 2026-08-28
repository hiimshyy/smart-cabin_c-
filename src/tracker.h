#pragma once
// Multi-object tracker for person detections, with face-recognition
// caching per track. Designed for elevator scene: 1-6 persons, low motion,
// long face-invisible periods (people facing the door).
//
// Algorithm:
//   - Greedy IoU association between new detections and existing tracks
//   - State machine: NEW -> CONFIRMED -> LOST -> KILLED
//   - Ghost list: killed CONFIRMED tracks with a recognized name remain
//     for `ghost_max_age_frames` so a re-appearing person can inherit the
//     original track_id and cumulative time.

#include <vector>
#include <string>
#include <cstdint>

#include "yolo_post.h"     // PersonDet

enum class TrackState : uint8_t {
    NEW,        // First seen, not yet confirmed
    CONFIRMED,  // Confirmed after `confirm_hits` consecutive matches
    LOST,       // Was confirmed but missed for < max_missed frames
};

struct Track {
    int         id = -1;
    float       x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float       conf = 0.0f;

    TrackState  state = TrackState::NEW;
    int         consecutive_hits = 0;
    int         missed_frames    = 0;
    int         first_seen_frame = -1;
    int         last_seen_frame  = -1;

    // Recognition state
    std::string name;                  // "" = never tried, "unknown" = tried no match
    float       match_sim      = -1.0f;
    int         last_recog_frame = -1;

    bool alive = true;

    float width()  const { return x2 - x1; }
    float height() const { return y2 - y1; }
    float area()   const { float w = width(), h = height(); return (w > 0 && h > 0) ? w * h : 0.0f; }
};

class Tracker {
public:
    // ---- Config (public — tune from CLI) ------------------------------
    float iou_thresh          = 0.3f;
    int   max_missed          = 30;      // ~1s @ 30 FPS before KILL
    int   confirm_hits        = 3;       // NEW -> CONFIRMED after N hits
    int   recog_retry_frames  = 90;      // Re-verify recognition every N frames
    int   recog_unknown_retry = 30;      // Retry "unknown" every N frames
    int   ghost_max_age_frames = 900;    // Ghost lives 30s @ 30 FPS

    // Face-to-track association: face center must lie within upper `head_ratio`
    // vertical fraction of the person bbox (heads are on top).
    float head_ratio = 0.5f;

    // ---- API ----------------------------------------------------------
    // Advance tracker one frame with new person detections.
    void update(const std::vector<PersonDet>& dets, int frame_idx);

    // Get all currently CONFIRMED or LOST tracks (i.e. drawable + eligible for recog).
    std::vector<Track*> active_tracks();

    // Find the smallest CONFIRMED track whose bbox contains (cx, cy) in the
    // upper `head_ratio` portion. Returns nullptr if no match.
    Track* find_track_for_face(float cx, float cy);

    // Record a recognition result on a track by ID. `name.empty()` won't
    // update, use "unknown" when threshold not met.
    void record_recognition(int track_id, const std::string& name,
                            float sim, int frame_idx);

    // Decide whether a track needs recognition this frame.
    bool needs_recog(const Track& t, int frame_idx) const;

    // Read-only view of internal tracks (includes NEW state).
    const std::vector<Track>& all_tracks() const { return tracks_; }

    // Diagnostics
    int  active_count() const;

private:
    struct Ghost {
        int         id;
        std::string name;
        int         first_seen_frame;
        int         killed_frame;
        // Last known bbox — could help spatial matching (not used in v1)
        float x1, y1, x2, y2;
    };

    std::vector<Track> tracks_;
    std::vector<Ghost> ghosts_;
    int                next_id_ = 1;

    // Called when we transition a track into a fresh CONFIRMED state and
    // it just got its first successful recognition — try to inherit
    // identity from a matching ghost.
    void try_inherit_identity(Track& t, int frame_idx);

    // Kill a track. If it had a name (recognized), push to ghosts_.
    void kill_track(size_t idx, int frame_idx);

    // Expire ghosts older than ghost_max_age_frames.
    void expire_ghosts(int frame_idx);
};

// Simple IoU helper (shared between tracker + tests).
float bbox_iou(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2);
