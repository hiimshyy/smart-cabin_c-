#pragma once

#include <cstdint>
#include <string>

// Data-only tracker types.  Consumers use TrackView snapshots instead of
// pointers/references into a backend-owned container.
enum class TrackerMode : uint8_t { Greedy, Byte };
enum class ByteTrackState : uint8_t { Tentative, Tracked, Lost, Removed };

struct BoxF {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct FrameContext {
    int frame_id = 0;
    double timestamp_ms = 0.0;  // monotonic/steady clock only
    int frame_width = 0;
    int frame_height = 0;
};

struct TrackIdentity {
    int64_t resident_id = -1;
    std::string display_name;
    float similarity = -1.0f;
    double last_recognition_ms = -1.0;
    double last_recognition_attempt_ms = -1.0;
    bool latched = false;
};

struct TrackView {
    int id = -1;
    ByteTrackState state = ByteTrackState::Tentative;
    BoxF box;
    float detection_score = 0.0f;
    bool predicted_only = false;
    TrackIdentity identity;
};

struct TrackerConfig {
    float high_threshold = 0.50f;
    float low_threshold = 0.10f;
    float primary_iou_gate = 0.30f;
    float secondary_iou_gate = 0.20f;
    int confirm_hits = 3;
    int lost_grace_min_updates = 2;
    double lost_grace_ms = 250.0;
    double lost_timeout_ms = 1200.0;
    double max_predict_gap_ms = 2000.0;
    double unknown_retry_ms = 1000.0;
};
