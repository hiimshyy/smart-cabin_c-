#include <cstdio>
#include <vector>

#include "byte_tracker.h"

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static PersonDet det(float x, float score = 0.9f) {
    return {x, 10.0f, x + 80.0f, 210.0f, score};
}
static FrameContext frame(int id, double t) { return {id, t, 640, 480}; }

int main() {
    TrackerConfig cfg;
    cfg.confirm_hits = 2;
    cfg.lost_grace_min_updates = 2;
    cfg.lost_grace_ms = 100.0;
    cfg.lost_timeout_ms = 500.0;
    cfg.unknown_retry_ms = 100.0;
    ByteTracker tracker(cfg);

    tracker.predict(frame(0, 0)); tracker.update({det(10)}, frame(0, 0));
    tracker.predict(frame(1, 50)); tracker.update({det(12)}, frame(1, 50));
    auto active = tracker.active_tracks();
    CHECK(active.size() == 1 && active[0].state == ByteTrackState::Tracked, "high detections confirm track");
    const int id = active.empty() ? -1 : active[0].id;

    tracker.predict(frame(2, 100)); tracker.update({}, frame(2, 100));
    active = tracker.active_tracks();
    CHECK(active.size() == 1 && active[0].state == ByteTrackState::Tracked && active[0].predicted_only,
          "one detector miss remains tracked grace");
    tracker.predict(frame(3, 160)); tracker.update({}, frame(3, 160));
    active = tracker.active_tracks();
    CHECK(active.size() == 1 && active[0].state == ByteTrackState::Lost, "grace expiry enters Lost");
    CHECK(tracker.tracked_count() == 0 && tracker.active_count() == 1,
          "Lost appears in active views but is excluded from tracked count");
    tracker.predict(frame(4, 200)); tracker.update({det(18)}, frame(4, 200));
    active = tracker.active_tracks();
    CHECK(active.size() == 1 && active[0].id == id && active[0].state == ByteTrackState::Tracked,
          "high detection reactivates Lost with same ID");

    tracker.record_recognition(id, 42, "private-label", 0.9f, 200);
    CHECK(!tracker.needs_recognition(id, 300), "known identity latches recognition");
    tracker.record_recognition(id, -1, "", 0.1f, 300);
    auto known = tracker.find_track(id);
    CHECK(known && known->identity.resident_id == 42 && known->identity.display_name == "private-label",
          "unknown result cannot overwrite known identity");

    ByteTracker low_only(cfg);
    low_only.predict(frame(0, 0)); low_only.update({det(10, 0.2f)}, frame(0, 0));
    CHECK(low_only.active_tracks().empty(), "low detection never creates a track");

    tracker.predict(frame(5, 800));
    CHECK(tracker.active_tracks().empty(), "absolute timeout removes even without detector update");
    tracker.predict(frame(6, 850)); tracker.update({det(20)}, frame(6, 850));
    tracker.predict(frame(7, 900)); tracker.update({det(22)}, frame(7, 900));
    active = tracker.active_tracks();
    CHECK(active.size() == 1 && active[0].id != id, "re-entry after Removed receives fresh ID");

    std::printf("\n[test_byte_tracker] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
