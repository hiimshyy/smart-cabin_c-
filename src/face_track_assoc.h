#pragma once

#include <vector>

#include "detection.h"
#include "tracker_types.h"

struct FaceTrackAssocConfig {
    float head_ratio = 0.50f;
    float head_margin_ratio = 0.05f;
    float min_face_width_ratio = 0.05f;
    float max_face_width_ratio = 0.80f;
    float min_face_height_ratio = 0.03f;
    float max_face_height_ratio = 0.60f;
};

// Return one track ID for each input face (-1 for orphan).  Only a fresh
// Tracked view (not predicted-only) participates.  The solver considers all
// faces/tracks together, so a track receives at most one face per frame.
std::vector<int> associate_faces_to_tracks(const std::vector<Detection>& faces,
                                           const std::vector<TrackView>& tracks,
                                           const FaceTrackAssocConfig& config = {});
