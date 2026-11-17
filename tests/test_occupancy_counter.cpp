#include "occupancy_counter.h"

#include <cstdio>
#include <utility>
#include <vector>

static int checks = 0;
static int failed = 0;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", message, __LINE__);                              \
        }                                                                                          \
    } while (0)

static OccupancyConfig make_config(float minimum_displacement = 0.0f) {
    OccupancyConfig config;
    CountingZone    zone;
    zone.name    = "door";
    zone.polygon = {{0.30f, 0.40f}, {0.70f, 0.40f}, {0.70f, 0.60f}, {0.30f, 0.60f}};
    zone.inside_direction_degrees  = 90.0f;
    zone.min_crossing_displacement = minimum_displacement;
    config.zones.push_back(std::move(zone));
    return config;
}

static TrackView track(int id, float center_x, float foot_y, bool predicted = false) {
    TrackView view;
    view.id             = id;
    view.state          = ByteTrackState::Tracked;
    view.predicted_only = predicted;
    view.box = {(center_x - 0.05f) * 640.0f, (foot_y - 0.16f) * 480.0f, (center_x + 0.05f) * 640.0f,
                foot_y * 480.0f};
    return view;
}

static TrackView track_with_first_observation(int id, float center_x, float first_foot_y,
                                              float  current_foot_y,
                                              double first_observation_ms = 0.0) {
    TrackView view             = track(id, center_x, current_foot_y);
    view.first_observation_box = track(id, center_x, first_foot_y).box;
    view.first_observation_ms  = first_observation_ms;
    view.has_first_observation = true;
    return view;
}

static OccupancySnapshot update(OccupancyCounter& counter, std::vector<TrackView> tracks,
                                double& timestamp, bool reliable = true) {
    timestamp += 16.0;
    return counter.update(tracks, 640, 480, timestamp, true, !reliable);
}

static void enter(OccupancyCounter& counter, int id, double& timestamp) {
    update(counter, {track(id, 0.5f, 0.35f)}, timestamp);
    update(counter, {track(id, 0.5f, 0.50f)}, timestamp);
    update(counter, {track(id, 0.5f, 0.70f)}, timestamp);
}

static OccupancySnapshot exit_track(OccupancyCounter& counter, int id, double& timestamp) {
    update(counter, {track(id, 0.5f, 0.70f)}, timestamp);
    update(counter, {track(id, 0.5f, 0.50f)}, timestamp);
    return update(counter, {track(id, 0.5f, 0.35f)}, timestamp);
}

static void test_entry_exit_duplicate_and_abort() {
    std::printf("test_entry_exit_duplicate_and_abort\n");
    OccupancyCounter counter(make_config(), OccupancyState{});
    double           timestamp = 0.0;
    enter(counter, 1, timestamp);
    CHECK(counter.count() == 1 && counter.confidence() == OccupancyConfidence::Certain,
          "outside-zone-inside validates entry");

    auto snapshot = update(counter, {track(1, 0.5f, 0.50f)}, timestamp);
    snapshot      = update(counter, {track(1, 0.5f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.empty(), "inside bounce is not double counted");
    CHECK(snapshot.confidence == OccupancyConfidence::Uncertain,
          "aborted exit marks confidence uncertain");

    snapshot = update(counter, {track(1, 0.5f, 0.50f)}, timestamp);
    snapshot = update(counter, {track(1, 0.5f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Exit,
          "inside-zone-outside validates exit");
    CHECK(snapshot.confidence == OccupancyConfidence::Certain, "validated exit restores certainty");

    OccupancyCounter aborted(make_config(0.10f), OccupancyState{});
    timestamp = 0.0;
    update(aborted, {track(2, 0.5f, 0.35f)}, timestamp);
    update(aborted, {track(2, 0.5f, 0.50f)}, timestamp);
    snapshot = update(aborted, {track(2, 0.5f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty(), "aborted entry does not count");
    CHECK(snapshot.confidence == OccupancyConfidence::Uncertain, "aborted entry stays uncertain");
}

static void test_loss_prediction_outage_and_degraded() {
    std::printf("test_loss_prediction_outage_and_degraded\n");
    OccupancyState initial;
    initial.count      = 2;
    initial.confidence = OccupancyConfidence::Certain;
    OccupancyCounter outage(make_config(), initial);
    auto             snapshot = outage.update({}, 0, 0, 1000.0, false, false);
    CHECK(snapshot.count == 2 && snapshot.confidence == OccupancyConfidence::Uncertain,
          "invalid frame marks outage without resetting count");

    OccupancyCounter predicted(make_config(), initial);
    double           timestamp = 0.0;
    update(predicted, {track(3, 0.5f, 0.35f)}, timestamp);
    snapshot = update(predicted, {track(3, 0.5f, 0.35f, true)}, timestamp);
    CHECK(snapshot.count == 2 && snapshot.confidence == OccupancyConfidence::Uncertain &&
              snapshot.pipeline_unreliable,
          "prediction-only observation marks uncertainty even outside zone");

    OccupancyCounter zone_loss(make_config(), initial);
    timestamp = 0.0;
    update(zone_loss, {track(4, 0.5f, 0.50f)}, timestamp);
    snapshot = update(zone_loss, {}, timestamp);
    CHECK(snapshot.count == 2 && snapshot.confidence == OccupancyConfidence::Uncertain,
          "track lost in zone does not change count and marks uncertainty");

    OccupancyCounter inside_loss(make_config(), OccupancyState{});
    timestamp = 0.0;
    enter(inside_loss, 5, timestamp);
    snapshot = update(inside_loss, {}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.confidence == OccupancyConfidence::Uncertain,
          "track loss inside retains occupancy");

    OccupancyCounter degraded(make_config(), initial);
    timestamp = 0.0;
    snapshot  = update(degraded, {track(6, 0.5f, 0.35f)}, timestamp, false);
    CHECK(snapshot.count == 2 && snapshot.confidence == OccupancyConfidence::Uncertain &&
              snapshot.pipeline_unreliable,
          "degraded tracking marks uncertainty without reset");
}

static void test_id_churn() {
    std::printf("test_id_churn\n");
    int              callbacks = 0;
    OccupancyCounter clear(make_config(), OccupancyState{},
                           [&](int, OccupancyConfidence) { ++callbacks; });
    double           timestamp = 0.0;
    enter(clear, 1, timestamp);
    auto snapshot = update(clear, {track(2, 0.51f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.confidence == OccupancyConfidence::Certain &&
              snapshot.events.empty(),
          "single nearby successor transfers without duplicate entry or lost certainty");
    CHECK(callbacks == 1, "clean ID handoff does not schedule an unchanged state");

    OccupancyState certain;
    certain.confidence = OccupancyConfidence::Certain;
    OccupancyCounter ambiguous(make_config(), certain);
    timestamp = 0.0;
    update(ambiguous, {track(10, 0.49f, 0.35f), track(11, 0.51f, 0.35f)}, timestamp);
    update(ambiguous, {}, timestamp);
    snapshot = update(ambiguous, {track(12, 0.50f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.confidence == OccupancyConfidence::Uncertain,
          "multiple matching ghosts are treated as ambiguous");
}

static void test_one_to_many_churn_and_partial_first_observation() {
    std::printf("test_one_to_many_churn_and_partial_first_observation\n");
    OccupancyState certain;
    certain.confidence = OccupancyConfidence::Certain;

    int              split_callbacks = 0;
    OccupancyCounter split(make_config(), certain,
                           [&](int, OccupancyConfidence) { ++split_callbacks; });
    double           timestamp = 0.0;
    update(split, {track(20, 0.50f, 0.35f)}, timestamp);
    auto snapshot = update(split, {track(21, 0.49f, 0.35f), track(22, 0.51f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "one predecessor with multiple successors marks uncertainty without an event");
    CHECK(split_callbacks == 1, "one-to-many churn schedules one confidence persistence update");

    update(split, {track(21, 0.49f, 0.50f), track(22, 0.51f, 0.50f)}, timestamp);
    snapshot = update(split, {track(21, 0.49f, 0.70f), track(22, 0.51f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "ambiguous successor crossings cannot duplicate an entry");
    CHECK(split_callbacks == 1, "suppressed ambiguous crossings do not schedule duplicate state");

    int              ghost_callbacks = 0;
    OccupancyCounter ghost_split(make_config(), certain,
                                 [&](int, OccupancyConfidence) { ++ghost_callbacks; });
    timestamp = 0.0;
    update(ghost_split, {track(40, 0.50f, 0.35f)}, timestamp);
    update(ghost_split, {}, timestamp);
    snapshot = update(ghost_split, {track(41, 0.49f, 0.35f), track(42, 0.51f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain && ghost_callbacks == 1,
          "one ghost with multiple successors is rejected before handoff");
    update(ghost_split, {track(41, 0.49f, 0.50f), track(42, 0.51f, 0.50f)}, timestamp);
    snapshot = update(ghost_split, {track(41, 0.49f, 0.70f), track(42, 0.51f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain && ghost_callbacks == 1,
          "rejected ghost successors cannot emit duplicate crossings");

    int              partial_callbacks = 0;
    OccupancyCounter partial(make_config(), certain,
                             [&](int, OccupancyConfidence) { ++partial_callbacks; });
    timestamp = 0.0;
    snapshot  = update(partial, {track(30, 0.50f, 0.50f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "track first observed in zone preserves count and marks uncertainty");
    CHECK(partial_callbacks == 1, "zone-first observation schedules confidence persistence");
    snapshot = update(partial, {track(30, 0.50f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "zone-first to inside does not invent an entry or restore certainty");
    CHECK(partial_callbacks == 1, "zone-first transition does not schedule unchanged state");
}

static void test_candidate_survives_sticky_band() {
    std::printf("test_candidate_survives_sticky_band\n");
    OccupancyCounter counter(make_config(0.05f), OccupancyState{});
    double           timestamp = 0.0;
    update(counter, {track(100, 0.5f, 0.35f)}, timestamp);
    update(counter, {track(100, 0.5f, 0.50f)}, timestamp);
    auto snapshot = update(counter, {track(100, 0.75f, 0.55f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty(),
          "lateral sticky-band observation does not abort entry candidate");
    snapshot = update(counter, {track(100, 0.75f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Entry,
          "entry candidate completes after crossing the sticky band");

    snapshot = update(counter, {track(100, 0.5f, 0.50f)}, timestamp);
    snapshot = update(counter, {track(100, 0.75f, 0.55f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.empty(),
          "lateral sticky-band observation does not abort exit candidate");
    snapshot = update(counter, {track(100, 0.75f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Exit,
          "exit candidate completes after crossing the sticky band");
}

static void test_tentative_first_observation_crossing() {
    std::printf("test_tentative_first_observation_crossing\n");
    OccupancyCounter direct(make_config(0.05f), OccupancyState{});
    double           timestamp = 0.0;
    auto             snapshot =
        update(direct, {track_with_first_observation(90, 0.5f, 0.35f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Entry,
          "confirmed track recovers fast entry from its tentative first observation");

    OccupancyCounter staged(make_config(0.05f), OccupancyState{});
    timestamp = 0.0;
    snapshot  = update(staged, {track_with_first_observation(91, 0.5f, 0.35f, 0.50f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty(),
          "tentative outside-to-zone history starts an entry candidate");
    snapshot = update(staged, {track(91, 0.5f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Entry,
          "entry candidate recovered from tentative history completes inside");

    OccupancyState certain;
    certain.confidence = OccupancyConfidence::Certain;
    OccupancyCounter stale(make_config(0.05f), certain);
    timestamp = 2000.0;
    snapshot  = update(stale, {track_with_first_observation(92, 0.5f, 0.35f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "stale first-observation history cannot invent a crossing after reactivation");
}

static void test_skipped_zone_frame_crossing() {
    std::printf("test_skipped_zone_frame_crossing\n");
    OccupancyCounter counter(make_config(0.05f), OccupancyState{});
    double           timestamp = 0.0;
    update(counter, {track(80, 0.5f, 0.35f)}, timestamp);
    auto snapshot = update(counter, {track(80, 0.5f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Entry,
          "outside-to-inside segment crossing survives a skipped zone frame");
    snapshot = update(counter, {track(80, 0.5f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Exit,
          "inside-to-outside segment crossing survives a skipped zone frame");

    OccupancyState certain;
    certain.confidence = OccupancyConfidence::Certain;
    OccupancyCounter detour(make_config(0.05f), certain);
    timestamp = 0.0;
    update(detour, {track(81, 0.1f, 0.35f)}, timestamp);
    snapshot = update(detour, {track(81, 0.1f, 0.70f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "side jump around the polygon is not treated as a crossing");
}

static void test_unassigned_tracks_cannot_decrement_occupancy() {
    std::printf("test_unassigned_tracks_cannot_decrement_occupancy\n");
    OccupancyState certain_empty;
    certain_empty.confidence = OccupancyConfidence::Certain;
    OccupancyCounter empty(make_config(), certain_empty);
    double           timestamp = 0.0;
    auto             snapshot  = exit_track(empty, 50, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "track first seen inside cannot emit exit from empty occupancy");

    OccupancyCounter overlapping(make_config(), OccupancyState{});
    timestamp = 0.0;
    enter(overlapping, 60, timestamp);
    update(overlapping, {track(60, 0.5f, 0.70f), track(61, 0.6f, 0.70f)}, timestamp);
    update(overlapping, {track(60, 0.5f, 0.70f), track(61, 0.6f, 0.50f)}, timestamp);
    snapshot = update(overlapping, {track(60, 0.5f, 0.70f), track(61, 0.6f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 1 && snapshot.events.empty() &&
              snapshot.confidence == OccupancyConfidence::Uncertain,
          "uncounted inside track cannot consume another track's occupancy");
    snapshot = update(overlapping, {track(60, 0.5f, 0.50f)}, timestamp);
    snapshot = update(overlapping, {track(60, 0.5f, 0.35f)}, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Exit,
          "counted entry track can still emit its matching exit");

    OccupancyState restored;
    restored.count      = 1;
    restored.confidence = OccupancyConfidence::Certain;
    OccupancyCounter restored_counter(make_config(), restored);
    timestamp = 0.0;
    snapshot  = exit_track(restored_counter, 70, timestamp);
    CHECK(snapshot.count == 0 && snapshot.events.size() == 1 &&
              snapshot.events.front().direction == OccupancyEventDirection::Exit,
          "restored occupancy slot can be assigned to an observed inside track");
}

static void test_callback_and_reconcile_persistence_contract() {
    std::printf("test_callback_and_reconcile_persistence_contract\n");
    int                 callbacks       = 0;
    int                 last_count      = -1;
    OccupancyConfidence last_confidence = OccupancyConfidence::Uncertain;
    OccupancyCounter    counter(make_config(), OccupancyState{},
                                [&](int count, OccupancyConfidence confidence) {
                                 ++callbacks;
                                 last_count      = count;
                                 last_confidence = confidence;
                             });
    double              timestamp = 0.0;
    enter(counter, 1, timestamp);
    CHECK(callbacks == 1 && last_count == 1 && last_confidence == OccupancyConfidence::Certain,
          "validated crossing schedules changed state");
    counter.update({}, 0, 0, timestamp + 1000.0, false, false);
    counter.update({}, 0, 0, timestamp + 2000.0, false, false);
    CHECK(callbacks == 2 && last_count == 1 && last_confidence == OccupancyConfidence::Uncertain,
          "outage transition schedules once and keeps count");

    counter.reconcile(4);
    CHECK(counter.count() == 4 && counter.confidence() == OccupancyConfidence::Certain &&
              callbacks == 3 && last_count == 4,
          "reconcile schedules persistence and restores certainty");
    counter.reconcile_empty();
    CHECK(counter.count() == 0 && counter.confidence() == OccupancyConfidence::Certain &&
              callbacks == 4 && last_count == 0,
          "reconcile_empty schedules persistence");
}

static void test_occupant_lineage_and_same_id_ghost_recovery() {
    std::printf("test_occupant_lineage_and_same_id_ghost_recovery\n");
    OccupancyCounter counter(make_config(), OccupancyState{});
    double           timestamp = 0.0;

    // 1. Entry assigns monotonic non-zero occupant_id and preserves it across direct churn + ghost
    // churn.
    update(counter, {track(1, 0.50f, 0.35f)}, timestamp);
    update(counter, {track(1, 0.50f, 0.50f)}, timestamp);
    auto snap = update(counter, {track(1, 0.50f, 0.70f)}, timestamp);
    CHECK(snap.events.size() == 1 && snap.events[0].occupant_id == 1 &&
              snap.track_occupants.size() == 1 && snap.track_occupants[0].first == 1 &&
              snap.track_occupants[0].second == 1,
          "entry assigns occupant_id=1 and exposes track_occupants");

    // Direct transfer 1 -> 2 keeps occupant_id=1.
    snap = update(counter, {track(2, 0.51f, 0.70f)}, timestamp);
    CHECK(snap.track_occupants.size() == 1 && snap.track_occupants[0].first == 2 &&
              snap.track_occupants[0].second == 1,
          "direct ID handoff preserves occupant_id");

    // Ghost handoff 2 -> 3 keeps occupant_id=1.
    update(counter, {}, timestamp);
    snap = update(counter, {track(3, 0.51f, 0.70f)}, timestamp);
    CHECK(snap.track_occupants.size() == 1 && snap.track_occupants[0].first == 3 &&
              snap.track_occupants[0].second == 1,
          "ghost ID handoff preserves occupant_id");

    // Exit emits the same occupant_id=1 and removes slot from track_occupants.
    update(counter, {track(3, 0.51f, 0.50f)}, timestamp);
    snap = update(counter, {track(3, 0.51f, 0.35f)}, timestamp);
    CHECK(snap.events.size() == 1 && snap.events[0].direction == OccupancyEventDirection::Exit &&
              snap.events[0].occupant_id == 1 && snap.track_occupants.empty(),
          "exit emits original occupant_id and clears track_occupants");

    // 2. Two close tracks that go prediction-only (or briefly missing) and return under SAME track
    // IDs
    //    must recover their slots/occupant_ids even when moved > 0.05 and standing within 0.05 of
    //    each other.
    enter(counter, 10, timestamp);
    snap = update(counter, {track(10, 0.50f, 0.70f), track(11, 0.51f, 0.35f)}, timestamp);
    update(counter, {track(10, 0.50f, 0.70f), track(11, 0.51f, 0.50f)}, timestamp);
    snap = update(counter, {track(10, 0.50f, 0.70f), track(11, 0.51f, 0.70f)}, timestamp);
    CHECK(snap.count == 2 && snap.track_occupants.size() == 2,
          "two tracks enter and receive distinct occupant_ids");

    // Both tracks become predicted_only for one frame -> moved to ghosts_ with their track_ids.
    update(counter, {track(10, 0.50f, 0.70f, true), track(11, 0.51f, 0.70f, true)}, timestamp);
    // Track 10 exits quickly (displacement > 0.05) while track 11 stays nearby (< 0.05 from old
    // pos).
    snap = update(counter, {track(10, 0.50f, 0.35f), track(11, 0.51f, 0.70f)}, timestamp);
    CHECK(snap.count == 1 && snap.events.size() == 1 &&
              snap.events[0].direction == OccupancyEventDirection::Exit &&
              snap.events[0].track_id == 10 && snap.events[0].occupant_id == 2,
          "same-ID ghost recovery preserves slot across >0.05 move and nearby track");
    snap = update(counter, {track(11, 0.51f, 0.35f)}, timestamp);
    CHECK(snap.count == 0 && snap.events.size() == 1 &&
              snap.events[0].direction == OccupancyEventDirection::Exit &&
              snap.events[0].track_id == 11 && snap.events[0].occupant_id == 3,
          "second track retains occupant_id and emits matching exit");

    // 3. Restored slot from persisted state yields occupant_id=0 on Exit.
    OccupancyState restored;
    restored.count      = 1;
    restored.confidence = OccupancyConfidence::Certain;
    OccupancyCounter restored_counter(make_config(), restored);
    double           ts2 = 0.0;
    snap                 = exit_track(restored_counter, 70, ts2);
    CHECK(snap.events.size() == 1 && snap.events[0].direction == OccupancyEventDirection::Exit &&
              snap.events[0].occupant_id == 0,
          "restored slot without observed entry emits occupant_id=0");
}

int main() {
    test_entry_exit_duplicate_and_abort();
    test_loss_prediction_outage_and_degraded();
    test_id_churn();
    test_one_to_many_churn_and_partial_first_observation();
    test_candidate_survives_sticky_band();
    test_tentative_first_observation_crossing();
    test_skipped_zone_frame_crossing();
    test_unassigned_tracks_cannot_decrement_occupancy();
    test_callback_and_reconcile_persistence_contract();
    test_occupant_lineage_and_same_id_ghost_recovery();
    std::printf("\n[test_occupancy_counter] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
