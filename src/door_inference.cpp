#include "door_inference.h"

DoorSignal decode_door_register(std::uint16_t value, bool fresh) {
    DoorSignal out;
    out.fresh = fresh;
    out.opening = (value & (1u << 5)) != 0;
    out.closing = (value & (1u << 6)) != 0;
    if (out.opening && out.closing) out.fresh = false; // contradictory board state
    return out;
}

const char* door_inference_state_name(DoorInferenceState state) {
    switch (state) {
        case DoorInferenceState::Idle: return "IDLE";
        case DoorInferenceState::DoorOpen: return "DOOR_OPEN";
        case DoorInferenceState::Occupied: return "OCCUPIED";
        case DoorInferenceState::EmptyGrace: return "EMPTY_GRACE";
        case DoorInferenceState::Degraded: return "DEGRADED";
    }
    return "?";
}

DoorRunPlan DoorInferenceScheduler::plan(double now_ms, DoorSignal door,
                                         DoorTrackSummary tracks) {
    if (tracks.active > 0) last_presence_ms_ = now_ms;
    if (!door.fresh) {
        door_known_ = false;
        door_open_latched_ = false;
    } else {
        if (door.opening) { door_known_ = true; door_open_latched_ = true; }
        if (door.closing) {
            door_known_ = true;
            if (door_open_latched_) last_close_ms_ = now_ms;
            door_open_latched_ = false;
        }
    }

    DoorInferenceState next = DoorInferenceState::Idle;
    if (!door.fresh || !door_known_) next = DoorInferenceState::Degraded;
    else if (door_open_latched_) next = DoorInferenceState::DoorOpen;
    else if (tracks.active > 0) next = DoorInferenceState::Occupied;
    else if ((last_presence_ms_ >= 0.0 && now_ms - last_presence_ms_ < 3000.0) ||
             (last_close_ms_ >= 0.0 && now_ms - last_close_ms_ < 3000.0))
        next = DoorInferenceState::EmptyGrace;

    const bool changed = !initialized_ || next != state_;
    initialized_ = true;
    state_ = next;
    if (changed) { next_person_ms_ = now_ms; next_face_ms_ = now_ms; }

    DoorRunPlan result;
    result.state = next;
    double person_interval = 0.0;
    switch (next) {
        case DoorInferenceState::Idle: break;
        case DoorInferenceState::DoorOpen: person_interval = 1000.0 / 15.0; break;
        case DoorInferenceState::Occupied: person_interval = tracks.unresolved > 0 ? 100.0 : 200.0; break;
        case DoorInferenceState::EmptyGrace: person_interval = 200.0; break;
        case DoorInferenceState::Degraded: person_interval = tracks.unresolved > 0 ? 100.0 : 500.0; break;
    }
    if (person_interval > 0.0 && now_ms >= next_person_ms_) {
        result.run_person = true;
        next_person_ms_ = now_ms + person_interval;
    }
    result.run_face = face_after_person(now_ms, tracks);
    return result;
}

bool DoorInferenceScheduler::face_after_person(double now_ms, DoorTrackSummary tracks) {
    if (tracks.unresolved > 0 &&
        (state_ == DoorInferenceState::Occupied || state_ == DoorInferenceState::Degraded) &&
        next_person_ms_ > now_ms + 100.0) next_person_ms_ = now_ms + 100.0;
    if (state_ == DoorInferenceState::Idle || tracks.unresolved <= 0 ||
        tracks.tracked <= 0 || now_ms < next_face_ms_) return false;
    next_face_ms_ = now_ms + 100.0; // 10 FPS cap; recognition remains per-track
    return true;
}
