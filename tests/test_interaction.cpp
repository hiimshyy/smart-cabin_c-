// Unit test for InteractionManager (spec resident-db-layer, Task 5.2).
// No NPU / no SQLite required — pure state-machine logic on a fake timeline.
// Build with:
//   g++ -std=c++17 -Isrc tests/test_interaction.cpp \
//       src/interaction.cpp src/match_engine.cpp -o test_interaction
//
// (match_engine.cpp is only needed for the MatchResult struct symbol
//  reference; the test doesn't actually build a MatchEngine.)

#include <cstdio>
#include <vector>

#include "interaction.h"

static int g_checks = 0;
static int g_fail   = 0;

#define CHECK(cond, msg)                                              \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                              \
            ++g_fail;                                               \
            std::printf("  FAIL: %s  (line %d)\n", msg, __LINE__);  \
        }                                                          \
    } while (0)

// Handy helpers to construct one-subject frames.
static std::vector<std::pair<int, MatchResult>>
frame(int key, int64_t rid, float sim) {
    MatchResult r;
    r.resident_id = rid;
    r.similarity  = sim;
    return {{key, r}};
}
static std::vector<std::pair<int, MatchResult>>
frame_empty() { return {}; }

int main() {
    std::printf("== InteractionManager tests ==\n");

    // ---- Baseline config: streak=3, cooldown=1000ms, unknown_after=500ms
    InteractionConfig cfg;
    cfg.confirm_streak   = 3;
    cfg.cooldown_ms      = 1000.0;
    cfg.unknown_after_ms = 500.0;

    // -----------------------------------------------------------------
    // 1) Empty update returns no outcomes and creates no sessions.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        auto out = m.update(frame_empty(), 0.0);
        CHECK(out.empty(), "empty frame -> no outcomes");
        CHECK(m.session_count() == 0, "empty frame -> no sessions");
    }

    // -----------------------------------------------------------------
    // 2) Streak reaches confirm_streak -> single 'confirmed' outcome.
    //    Prior frames stay silent.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        // frame 1 & 2: matched but not enough to confirm
        for (int i = 0; i < 2; ++i) {
            auto out = m.update(frame(/*key=*/7, /*rid=*/42, 0.80f),
                                (double)(i * 33));
            CHECK(out.empty(), "streak below threshold -> no outcome");
        }
        // frame 3 hits confirm_streak
        auto out = m.update(frame(7, 42, 0.82f), 66.0);
        CHECK(out.size() == 1, "streak reached -> exactly 1 outcome");
        CHECK(out.size() > 0 && out[0].confirmed,
              "outcome is confirmed==true");
        CHECK(out.size() > 0 && out[0].resident_id == 42, "outcome resident_id=42");
        CHECK(out.size() > 0 && out[0].similarity == 0.82f,
              "outcome similarity is best-seen");
        CHECK(out.size() > 0 && out[0].subject_key == 7, "outcome subject_key=7");
    }

    // -----------------------------------------------------------------
    // 3) After CONFIRMED, subsequent same-resident frames stay silent.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        for (int i = 0; i < 3; ++i) m.update(frame(1, 100, 0.75f), i * 30.0);
        // Now CONFIRMED; keep feeding matches, should be silent.
        for (int i = 0; i < 20; ++i) {
            auto out = m.update(frame(1, 100, 0.76f), 100.0 + i * 30.0);
            CHECK(out.empty(), "confirmed then continuing -> no repeat outcome");
        }
    }

    // -----------------------------------------------------------------
    // 4) Cooldown: same resident confirmed within cooldown_ms → suppressed.
    //    A DIFFERENT subject_key (e.g. person leaves + re-enters) should not
    //    retrigger during cooldown.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        // Session A confirms at ~90ms.
        for (int i = 0; i < 3; ++i) m.update(frame(1, 55, 0.70f), i * 30.0);
        // Subject 1 disappears (session dropped).
        m.update(frame_empty(), 150.0);
        // Subject 2 arrives shortly after, same resident 55.
        int confirmed_count = 0;
        for (int i = 0; i < 3; ++i) {
            auto out = m.update(frame(2, 55, 0.72f), 200.0 + i * 30.0);
            for (const auto& o : out) if (o.confirmed) ++confirmed_count;
        }
        CHECK(confirmed_count == 0,
              "same resident within cooldown -> no new confirmed outcome");
    }

    // -----------------------------------------------------------------
    // 5) After cooldown elapses, same resident can be confirmed again.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        for (int i = 0; i < 3; ++i) m.update(frame(1, 77, 0.70f), i * 30.0);
        // ~66ms: session confirmed. Drop and re-arrive AFTER cooldown_ms.
        m.update(frame_empty(), 200.0);
        // Wait past cooldown (1000ms from first confirm ~66ms → past 1066ms).
        int confirmed_count = 0;
        for (int i = 0; i < 3; ++i) {
            auto out = m.update(frame(3, 77, 0.71f), 1100.0 + i * 30.0);
            for (const auto& o : out) if (o.confirmed) ++confirmed_count;
        }
        CHECK(confirmed_count == 1,
              "past cooldown -> new confirmed outcome allowed");
    }

    // -----------------------------------------------------------------
    // 6) Unknown timeout: subject present with no match for
    //    unknown_after_ms -> single 'unknown' outcome, not repeated.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        // Below-threshold matches are reported as resident_id=-1.
        auto out1 = m.update(frame(9, -1, 0.10f), 0.0);
        CHECK(out1.empty(), "unknown before timeout -> silent");
        auto out2 = m.update(frame(9, -1, 0.10f), 200.0);
        CHECK(out2.empty(), "still under unknown_after_ms -> silent");
        // Cross the threshold (>= 500ms).
        auto out3 = m.update(frame(9, -1, 0.10f), 500.0);
        CHECK(out3.size() == 1, "unknown timeout crossed -> 1 outcome");
        CHECK(out3.size() > 0 && out3[0].unknown, "outcome unknown==true");
        CHECK(out3.size() > 0 && out3[0].resident_id == -1,
              "unknown outcome resident_id == -1");
        CHECK(out3.size() > 0 && out3[0].subject_key == 9,
              "unknown outcome carries subject_key");

        // Further frames must not re-emit unknown for this session.
        auto out4 = m.update(frame(9, -1, 0.10f), 800.0);
        auto out5 = m.update(frame(9, -1, 0.10f), 1200.0);
        CHECK(out4.empty() && out5.empty(),
              "unknown emitted only once per session");
    }

    // -----------------------------------------------------------------
    // 7) Subject disappears -> session dropped; re-appearing key starts
    //    a fresh session with a fresh first_seen_ms window.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        m.update(frame(5, -1, 0.0f), 0.0);
        m.update(frame(5, -1, 0.0f), 200.0);
        CHECK(m.session_count() == 1, "session present after two frames");
        m.update(frame_empty(), 300.0);
        CHECK(m.session_count() == 0, "missing subject -> session dropped");
        m.update(frame(5, -1, 0.0f), 400.0);
        CHECK(m.session_count() == 1, "re-appearing subject -> new session");
        // Old first_seen_ms was 0; new session should start at 400 so that
        // unknown_timeout at 500ms window is not yet elapsed here.
        auto out = m.update(frame(5, -1, 0.0f), 800.0);
        CHECK(out.empty(),
              "new session's unknown window measured from new first_seen");
        auto out2 = m.update(frame(5, -1, 0.0f), 950.0);
        CHECK(out2.size() == 1 && out2[0].unknown,
              "new session emits unknown after its own timeout");
    }

    // -----------------------------------------------------------------
    // 8) Resident switch mid-session resets the streak.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        m.update(frame(1, 100, 0.80f), 0.0);
        m.update(frame(1, 100, 0.80f), 30.0);
        // 2/3 toward confirm for resident 100.
        auto out = m.update(frame(1, 200, 0.80f), 60.0);
        CHECK(out.empty(), "switch resident mid-streak -> no confirm yet");
        // Now build streak for resident 200 — need 3 more frames.
        m.update(frame(1, 200, 0.80f), 90.0);
        auto out2 = m.update(frame(1, 200, 0.85f), 120.0);
        CHECK(out2.size() == 1 && out2[0].resident_id == 200,
              "streak of new resident confirms correctly");
    }

    // -----------------------------------------------------------------
    // 9) Two independent subjects tracked separately.
    // -----------------------------------------------------------------
    {
        InteractionManager m(cfg);
        std::vector<std::pair<int, MatchResult>> two;
        MatchResult r_a; r_a.resident_id = 111; r_a.similarity = 0.9f;
        MatchResult r_b; r_b.resident_id = 222; r_b.similarity = 0.8f;
        two.push_back({1, r_a});
        two.push_back({2, r_b});

        m.update(two, 0.0);
        m.update(two, 30.0);
        auto out = m.update(two, 60.0);
        CHECK(out.size() == 2, "both subjects confirm on same frame");
        bool saw111 = false, saw222 = false;
        for (const auto& o : out) {
            if (o.resident_id == 111) saw111 = true;
            if (o.resident_id == 222) saw222 = true;
        }
        CHECK(saw111 && saw222, "each subject gets its own confirmed outcome");
        CHECK(m.session_count() == 2, "two concurrent sessions");
    }

    std::printf("\n[test_interaction] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
