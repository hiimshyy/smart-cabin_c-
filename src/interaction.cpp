#include "interaction.h"

#include <set>

const char* InteractionManager::state_name(SessionState s) {
    switch (s) {
        case SessionState::DETECTING: return "DETECTING";
        case SessionState::MATCHED:   return "MATCHED";
        case SessionState::CONFIRMED: return "CONFIRMED";
    }
    return "?";
}

void InteractionManager::reset() {
    sessions_.clear();
    last_confirmed_ms_.clear();
}

std::vector<InteractionManager::SessionView>
InteractionManager::sessions() const {
    std::vector<SessionView> out;
    out.reserve(sessions_.size());
    for (const auto& [key, s] : sessions_) {
        out.push_back({key, s.state, s.resident_id, s.best_sim,
                       s.matched_streak, s.first_seen_ms});
    }
    return out;
}

std::vector<InteractionManager::Outcome>
InteractionManager::update(
    const std::vector<std::pair<int, MatchResult>>& subjects,
    double now_ms) {

    std::vector<Outcome> outcomes;

    // ---- 1. Reap sessions whose subjects disappeared ------------------
    std::set<int> present;
    for (const auto& [key, _] : subjects) present.insert(key);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (present.count(it->first) == 0) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    // ---- 2. Drive each present subject's session ----------------------
    for (const auto& [key, res] : subjects) {
        // find-or-insert so we can distinguish "just created" from "exists"
        auto it = sessions_.find(key);
        const bool is_new = (it == sessions_.end());
        if (is_new) {
            Session s;
            s.subject_key    = key;
            s.first_seen_ms  = now_ms;
            s.last_seen_ms   = now_ms;
            s.state          = SessionState::DETECTING;
            it = sessions_.emplace(key, s).first;
        }
        Session& sess = it->second;
        sess.last_seen_ms = now_ms;

        const int64_t matched_id = res.resident_id;
        const float   sim        = res.similarity;

        // ---- 2a. This frame produced NO match (below threshold) -----
        if (matched_id < 0) {
            // Coming from MATCHED / CONFIRMED means we lost the ID.
            // Fall back toward DETECTING and clear resident bookkeeping.
            if (sess.state != SessionState::DETECTING) {
                sess.state          = SessionState::DETECTING;
                sess.resident_id    = -1;
                sess.matched_streak = 0;
                sess.best_sim       = -1.0f;
            }
            // Emit 'unknown' at most once per session, only for sessions
            // that stayed unmatched for the full window. We intentionally
            // measure from first_seen_ms so pre-existing sessions that
            // just lost their match don't spuriously fire unknown right
            // away (they had a legitimate identity earlier).
            if (!sess.unknown_emitted &&
                (now_ms - sess.first_seen_ms) >= cfg_.unknown_after_ms) {
                Outcome oc;
                oc.subject_key = key;
                oc.unknown     = true;
                outcomes.push_back(oc);
                sess.unknown_emitted = true;
            }
            continue;
        }

        // ---- 2b. This frame matched a resident ----------------------
        if (sess.resident_id == matched_id) {
            // Same person as previous frame(s): extend the streak.
            sess.matched_streak++;
            if (sim > sess.best_sim) sess.best_sim = sim;
        } else {
            // First match, or resident switched: reset streak.
            sess.resident_id    = matched_id;
            sess.matched_streak = 1;
            sess.best_sim       = sim;
        }

        // Ensure we're at least in MATCHED once we have a resident.
        if (sess.state == SessionState::DETECTING) {
            sess.state = SessionState::MATCHED;
        }

        // ---- 2c. Streak reached the confirmation threshold ----------
        if (sess.matched_streak >= cfg_.confirm_streak &&
            sess.state != SessionState::CONFIRMED) {
            sess.state        = SessionState::CONFIRMED;
            sess.confirmed_ms = now_ms;

            // Cooldown gate: same resident confirmed too recently on any
            // subject → suppress the outcome but keep the state.
            auto cd_it       = last_confirmed_ms_.find(matched_id);
            const bool in_cd = (cd_it != last_confirmed_ms_.end()) &&
                               (now_ms - cd_it->second) < cfg_.cooldown_ms;
            if (!in_cd) {
                Outcome oc;
                oc.subject_key = key;
                oc.resident_id = matched_id;
                oc.similarity  = sess.best_sim;
                oc.confirmed   = true;
                outcomes.push_back(oc);
                last_confirmed_ms_[matched_id] = now_ms;
            }
        }
    }

    return outcomes;
}
