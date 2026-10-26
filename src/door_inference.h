#pragma once

#include <cstdint>

enum class DoorInferenceState { Idle, DoorOpen, Occupied, EmptyGrace, Degraded };

struct DoorSignal {
    bool fresh = false;
    bool opening = false;
    bool closing = false;
};

DoorSignal decode_door_register(std::uint16_t value, bool fresh);

struct DoorTrackSummary {
    int active = 0;
    int tracked = 0;
    int unresolved = 0;
};

struct DoorRunPlan {
    DoorInferenceState state = DoorInferenceState::Degraded;
    bool run_person = false;
    bool run_face = false;
};

const char* door_inference_state_name(DoorInferenceState state);

class DoorInferenceScheduler {
public:
    DoorRunPlan plan(double now_ms, DoorSignal door, DoorTrackSummary tracks);
    // Re-evaluate the face gate after YOLO creates/confirms a track this frame.
    bool face_after_person(double now_ms, DoorTrackSummary tracks);
private:
    DoorInferenceState state_ = DoorInferenceState::Degraded;
    bool initialized_ = false;
    bool door_known_ = false;
    bool door_open_latched_ = false;
    double next_person_ms_ = 0.0;
    double next_face_ms_ = 0.0;
    double last_presence_ms_ = -1.0;
    double last_close_ms_ = -1.0;
};
