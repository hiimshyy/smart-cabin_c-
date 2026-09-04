#pragma once
// Interaction session state machine (spec resident-db-layer, R4).
//
// The realtime frame loop is stateless: every frame produces detections
// and a MatchResult per face. That is too noisy to drive cabin actions
// (greeting, floor call, audit log): matches flicker between "known"
// and "unknown" as pose/lighting shifts, and one interaction may last
// hundreds of frames.
//
// InteractionManager groups those per-frame matches into stable
// "interaction sessions", one per subject (either a track_id when the
// tracker is enabled, or a stable face slot like index 0 = largest face
// otherwise). It emits at most a handful of Outcome events per session:
//
//   - `confirmed`: the same resident matched for `confirm_streak`
//     consecutive frames, AND that resident is not currently in the
//     cooldown window. Fires the "matched" match_events row.
//   - `unknown`  : the subject has been present for at least
//     `unknown_after_ms` without ever producing a match. Fires the
//     "unknown" match_events row exactly once per session.
//
// The manager owns no threads and no I/O; the caller is expected to
// forward Outcomes to ResidentDB::log_event() / touch_resident().

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "match_engine.h"      // MatchResult

enum class SessionState : std::uint8_t {
    DETECTING,   // subject present, no resident matched (or streak reset)
    MATCHED,     // matched a resident but streak < confirm_streak
    CONFIRMED,   // streak reached confirm_streak (event may or may not fire depending on cooldown)
};

struct InteractionConfig {
    int    confirm_streak    = 5;       // frames of consecutive same-resident match to CONFIRM
    double cooldown_ms       = 3000.0;  // suppress duplicate 'matched' events for same resident
    double unknown_after_ms  = 2000.0;  // emit one 'unknown' if subject stays that long unmatched
};

class InteractionManager {
public:
    struct Outcome {
        int     subject_key    = -1;
        int64_t resident_id    = -1;    // valid only when confirmed==true
        float   similarity     = -1.0f; // best sim across the streak (confirmed) or -1 (unknown)
        bool    confirmed      = false; // fire match_events(action='matched') + touch_resident
        bool    unknown        = false; // fire match_events(action='unknown', resident_id NULL)
    };

    // Read-only session view for debug overlay.
    struct SessionView {
        int          subject_key;
        SessionState state;
        int64_t      resident_id;
        float        best_sim;
        int          matched_streak;
        double       first_seen_ms;
    };

    InteractionManager() = default;
    explicit InteractionManager(const InteractionConfig& cfg) : cfg_(cfg) {}

    void                     set_config(const InteractionConfig& cfg) { cfg_ = cfg; }
    const InteractionConfig& config() const { return cfg_; }

    // Advance the state machine one frame. `subjects` contains one entry
    // per subject visible in this frame; subjects missing from the list
    // (compared to prior frames) are considered gone and their sessions
    // are dropped. Returns any Outcome events that fired this call.
    std::vector<Outcome> update(
        const std::vector<std::pair<int, MatchResult>>& subjects,
        double now_ms);

    // Reset all state (drop sessions and cooldown history).
    void reset();

    std::vector<SessionView> sessions() const;
    std::size_t              session_count() const { return sessions_.size(); }

    // Debug helper: string label for a state (used by tests + logs).
    static const char* state_name(SessionState s);

private:
    struct Session {
        int          subject_key    = -1;
        SessionState state          = SessionState::DETECTING;
        int64_t      resident_id    = -1;
        float        best_sim       = -1.0f;
        int          matched_streak = 0;
        double       first_seen_ms  = 0.0;
        double       last_seen_ms   = 0.0;
        double       confirmed_ms   = 0.0;
        bool         unknown_emitted = false;
    };

    InteractionConfig            cfg_;
    std::map<int, Session>       sessions_;
    std::map<int64_t, double>    last_confirmed_ms_;
};
