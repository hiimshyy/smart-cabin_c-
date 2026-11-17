#include <cassert>
#include <cstdio>
#include <vector>

#include "trip_assembler.h"

namespace {

int checks = 0;
int failed = 0;

#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

CabinStopRecord make_stop(const std::string& key, int floor, double arrived, double door_open,
                          double door_close, double departed, bool closed) {
    CabinStopRecord s;
    s.stop_key          = key;
    s.cabin_id          = 2;
    s.slave             = 2;
    s.floor             = floor;
    s.has_floor         = true;
    s.arrived_at_ms     = arrived;
    s.door_opened_at_ms = door_open;
    s.door_closed_at_ms = door_close;
    s.departed_at_ms    = departed;
    s.closed            = closed;
    return s;
}

TimelineEvent stop_ev(TimelineEventType type, const CabinStopRecord& stop, double ts) {
    TimelineEvent ev;
    ev.type         = type;
    ev.slave        = 2;
    ev.timestamp_ms = ts;
    ev.stop         = stop;
    return ev;
}

TimelineEvent run_ev(TimelineEventType type, const std::string& run_key,
                     const std::string& from_stop, int from_floor, const std::string& to_stop,
                     int to_floor, const std::string& dir, double departed, double arrived) {
    TimelineEvent ev;
    ev.type               = type;
    ev.slave              = 2;
    ev.timestamp_ms       = (type == TimelineEventType::RunStarted) ? departed : arrived;
    ev.run.run_key        = run_key;
    ev.run.cabin_id       = 2;
    ev.run.slave          = 2;
    ev.run.from_stop_key  = from_stop;
    ev.run.to_stop_key    = to_stop;
    ev.run.from_floor     = from_floor;
    ev.run.has_from_floor = from_floor >= 0;
    ev.run.to_floor       = to_floor;
    ev.run.has_to_floor   = to_floor >= 0;
    ev.run.direction      = dir;
    ev.run.departed_at_ms = departed;
    ev.run.arrived_at_ms  = arrived;
    ev.run.closed         = (type == TimelineEventType::RunEnded);
    return ev;
}

FaceSample make_sample(float v0, float v1, float quality, std::int64_t best_rid = -1,
                       float best_sim = -1.0f, bool with_crop = false) {
    FaceSample s;
    s.embedding.assign(4, 0.0f);
    s.embedding[0]      = v0;
    s.embedding[1]      = v1;
    s.quality           = quality;
    s.best_resident_id  = best_rid;
    s.best_resident_sim = best_sim;
    if (with_crop) {
        s.crop_bgr_112.assign(16, 42);
        s.crop_w = 112;
        s.crop_h = 112;
    }
    return s;
}

void test_normal_trip_pre_entry_identity_and_calls() {
    TripAssemblerConfig cfg;
    cfg.floor_tolerance_ms = 10000.0;
    cfg.cabin_capacity     = 15;
    cfg.visitor_reid       = true;
    cfg.save_faces         = TrafficSaveFaces::Visitors;
    TripAssembler ta(cfg);

    // Stop 1 opens at floor 1, t=1000..15000.
    auto s1_open = make_stop("s1", 1, 1000.0, 2000.0, -1.0, -1.0, false);
    ta.on_timeline_events({stop_ev(TimelineEventType::StopOpened, s1_open, 1000.0)}, 0,
                          OccupancyConfidence::Certain, 1000.0);

    // Hall call served at s1.
    HallCallRecord hc;
    hc.call_key        = "hc1";
    hc.floor           = 1;
    hc.direction       = "up";
    hc.served_stop_key = "s1";
    hc.outcome         = "served";
    ta.on_completed_calls({}, {hc}, 2500.0);

    // Pre-Entry face recognition confirmed 2s BEFORE Entry (still on track 10).
    ta.on_resident_confirmed(10, 42, 0.78f, 3000.0);
    ta.on_face_sample(10, make_sample(1.0f, 0.0f, 0.9f, 42, 0.78f, true), 3000.0);
    FloorCallDecision fcd;
    fcd.request_id  = "req-42";
    fcd.resident_id = 42;
    fcd.floor       = 5;
    fcd.state       = FloorCallState::AckedSuccess;
    ta.on_floor_call_decision(fcd, 3100.0);
    ta.on_auto_call_confirmations({FloorCallConfirmEvent{"req-42", 5, true}});

    // Entry at t=5000 with track 10 -> occupant 100.
    OccupancySnapshot snap_in;
    snap_in.count      = 1;
    snap_in.confidence = OccupancyConfidence::Certain;
    snap_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 10, 5000.0, 100});
    snap_in.track_occupants.push_back({10, 100});
    ta.on_occupancy(snap_in, 5000.0);

    // Car call bit 5 pressed while passenger is in cabin.
    ta.on_car_calls_mask(0, true);
    ta.on_car_calls_mask(static_cast<std::uint16_t>(1u << 5), true);

    // Cabin leaves s1 at 15000 and arrives at s2 (floor 5) at 30000.
    auto s1_closed = make_stop("s1", 1, 1000.0, 2000.0, 14000.0, 15000.0, true);
    auto out_dep   = ta.on_timeline_events(
          {stop_ev(TimelineEventType::StopClosed, s1_closed, 15000.0),
           run_ev(TimelineEventType::RunStarted, "r1", "s1", 1, "", -1, "up", 15000.0, -1.0)},
          1, OccupancyConfidence::Certain, 15000.0);
    CHECK(out_dep.run_passengers.size() == 1 && out_dep.run_passengers[0].run_key == "r1",
          "run_passengers captures occupant riding on r1");

    auto s2_open = make_stop("s2", 5, 30000.0, 31000.0, -1.0, -1.0, false);
    auto out_arr = ta.on_timeline_events(
        {run_ev(TimelineEventType::RunEnded, "r1", "s1", 1, "s2", 5, "up", 15000.0, 30000.0),
         stop_ev(TimelineEventType::StopOpened, s2_open, 30000.0)},
        1, OccupancyConfidence::Certain, 30000.0);
    CHECK(out_arr.completed_runs.size() == 1 && out_arr.completed_runs[0].has_load_count &&
              out_arr.completed_runs[0].load_count == 1 &&
              out_arr.completed_runs[0].load_confidence == "certain",
          "cabin run carries occupancy load_count and confidence");

    CarCallRecord cc;
    cc.call_key        = "cc5";
    cc.floor           = 5;
    cc.served_stop_key = "s2";
    cc.outcome         = "served";
    ta.on_completed_calls({cc}, {}, 32000.0);

    // Track handed off to track 11 for the same occupant 100 before Exit.
    OccupancySnapshot snap_out;
    snap_out.count      = 0;
    snap_out.confidence = OccupancyConfidence::Certain;
    snap_out.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 11, 33000.0, 100});
    auto out_exit = ta.on_occupancy(snap_out, 33000.0);

    CHECK(out_exit.completed_trips.size() == 1,
          "completed trip emitted immediately when inside stop");
    const auto& tr = out_exit.completed_trips[0];
    CHECK(tr.has_floor_in && tr.floor_in == 1 && tr.has_floor_out && tr.floor_out == 5,
          "trip assigned floor_in=1 and floor_out=5");
    CHECK(tr.identity_kind == "resident" && tr.resident_id == 42 && tr.called_floor == 5 &&
              tr.call_register_confirmed == 1,
          "pre-Entry resident confirmation and auto floor call linked");
    CHECK(tr.rides == 1 && tr.car_calls_mask == (1u << 5), "rides and car_calls_mask tracked");
    CHECK(tr.hall_call_key == "hc1" && tr.car_call_key == "cc5",
          "hall call and car call keys linked to boarded/alighted stops");
    CHECK(tr.confidence == "high" && tr.flags == 0, "clean trip has high confidence");
    CHECK(tr.face_samples[0].crop_bgr_112.empty(),
          "visitor-only save_faces drops crop for confirmed resident");

    // Collect stops flushed at arrival, Exit, and after poll past floor_tolerance_ms.
    std::vector<CabinStopRecord> flushed_stops = out_arr.completed_stops;
    flushed_stops.insert(flushed_stops.end(), out_exit.completed_stops.begin(),
                         out_exit.completed_stops.end());
    auto out_poll = ta.poll(40000.0);
    flushed_stops.insert(flushed_stops.end(), out_poll.completed_stops.begin(),
                         out_poll.completed_stops.end());
    CHECK(flushed_stops.size() == 1 && flushed_stops[0].stop_key == "s1" &&
              flushed_stops[0].boarded == 1 && flushed_stops[0].alighted == 0,
          "s1 flushes with boarded=1 after tolerance window");
}

void test_delayed_stop_and_adjacent_tie_break() {
    TripAssemblerConfig cfg;
    cfg.floor_tolerance_ms = 10000.0;
    TripAssembler ta(cfg);

    // s1: 1000..5000 (floor 1), s2: 9000..20000 (floor 3). Run 5000..9000.
    // Note: tol=10000 covers [5000..9000] from BOTH s1 (departed+10s=15000) and s2
    // (arrived-10s=-1000). At t=7000, distance to s1 is 2000 and distance to s2 is 2000 (exact
    // tie!).
    auto s1 = make_stop("s1", 1, 1000.0, 1500.0, 4500.0, 5000.0, true);
    ta.on_timeline_events(
        {stop_ev(TimelineEventType::StopOpened, s1, 1000.0),
         stop_ev(TimelineEventType::StopClosed, s1, 5000.0),
         run_ev(TimelineEventType::RunStarted, "r1", "s1", 1, "", -1, "up", 5000.0, -1.0)},
        0, OccupancyConfidence::Certain, 5000.0);

    // Delayed Entry at t=7000 (during run, equidistant to s1@5000 and future s2@9000).
    OccupancySnapshot snap_in;
    snap_in.count      = 1;
    snap_in.confidence = OccupancyConfidence::Certain;
    snap_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 1, 7000.0, 1});
    snap_in.track_occupants.push_back({1, 1});
    ta.on_occupancy(snap_in, 7000.0);

    // Delayed unobserved Exit at t=7000 while occupant 1 remains inside (count=1).
    OccupancySnapshot snap_ex;
    snap_ex.count      = 1;
    snap_ex.confidence = OccupancyConfidence::Certain;
    snap_ex.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 2, 7000.0, 0});
    auto out_early = ta.on_occupancy(snap_ex, 7000.0);
    CHECK(out_early.completed_trips.empty(),
          "unmatched exit does not resolve before tol or next stop arrives");

    // s2 opens at 9000 (floor 3): now both Entry@7000 and Exit@7000 tie (dist=2000 each).
    auto s2     = make_stop("s2", 3, 9000.0, 9500.0, -1.0, -1.0, false);
    auto out_s2 = ta.on_timeline_events(
        {run_ev(TimelineEventType::RunEnded, "r1", "s1", 1, "s2", 3, "up", 5000.0, 9000.0),
         stop_ev(TimelineEventType::StopOpened, s2, 9000.0)},
        1, OccupancyConfidence::Certain, 9000.0);

    CHECK(out_s2.completed_trips.size() == 1, "unmatched exit@7000 resolves once s2 opens at 9000");
    CHECK(out_s2.completed_trips[0].has_floor_out && out_s2.completed_trips[0].floor_out == 3,
          "tie-break Exit picks later stop s2 (floor 3)");
    CHECK((out_s2.completed_trips[0].flags & TrafficFlag::ENTRY_UNOBSERVED) != 0 &&
              out_s2.completed_trips[0].confidence == "low",
          "entry_unobserved flag produces low confidence");

    // Now Exit occupant 1 inside s2 at t=11000 -> Entry@7000 tie-breaks to earlier stop s1 (floor
    // 1).
    OccupancySnapshot snap_ex1;
    snap_ex1.count      = 0;
    snap_ex1.confidence = OccupancyConfidence::Certain;
    snap_ex1.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 1, 11000.0, 1});
    auto out_ex1 = ta.on_occupancy(snap_ex1, 11000.0);
    CHECK(out_ex1.completed_trips.size() == 1 && out_ex1.completed_trips[0].has_floor_in &&
              out_ex1.completed_trips[0].floor_in == 1 && out_ex1.completed_trips[0].floor_out == 3,
          "tie-break Entry picks earlier stop s1 (floor 1)");
}

void test_identity_conflict_no_ride_load_implausible_and_floor_unknown() {
    TripAssemblerConfig cfg;
    cfg.floor_tolerance_ms = 5000.0;
    cfg.cabin_capacity     = 15;
    TripAssembler ta(cfg);

    // Entry with NO stop within +/-5s -> waits tol then marks floor_unknown.
    OccupancySnapshot snap_in;
    snap_in.count      = 1;
    snap_in.confidence = OccupancyConfidence::Uncertain;
    snap_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 5, 1000.0, 50});
    snap_in.track_occupants.push_back({5, 50});
    ta.on_occupancy(snap_in, 1000.0);

    ta.on_resident_confirmed(5, 11, 0.55f, 1500.0);
    ta.on_resident_confirmed(5, 22, 0.81f, 2000.0);

    // Stop opens much later at t=20000 (floor 2). Both Entry and Exit inside s1 gives no_ride on
    // another test; here Exit occurs inside s1 at t=21000.
    auto s1 = make_stop("s1", 2, 20000.0, 20500.0, -1.0, -1.0, false);
    ta.on_timeline_events({stop_ev(TimelineEventType::StopOpened, s1, 20000.0)}, 1,
                          OccupancyConfidence::Certain, 20000.0);

    OccupancySnapshot snap_out;
    snap_out.count      = 0;
    snap_out.confidence = OccupancyConfidence::Certain;
    snap_out.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 5, 21000.0, 50});
    auto out_exit = ta.on_occupancy(snap_out, 21000.0);
    CHECK(out_exit.completed_trips.size() == 1, "unknown floor trip emitted");
    const auto& t = out_exit.completed_trips[0];
    CHECK(!t.has_floor_in && (t.flags & TrafficFlag::FLOOR_UNKNOWN) != 0 &&
              (t.flags & TrafficFlag::OCCUPANCY_UNCERTAIN_IN) != 0 && t.confidence == "low",
          "no stop in tol sets floor_unknown and preserves occupancy_uncertain_in");
    CHECK(t.identity_kind == "resident" && t.resident_id == 22 &&
              (t.flags & TrafficFlag::IDENTITY_CONFLICT) != 0,
          "highest similarity resident wins and sets identity_conflict");

    // Test no_ride and load_implausible
    OccupancySnapshot snap_nr_in;
    snap_nr_in.count      = 1;
    snap_nr_in.confidence = OccupancyConfidence::Certain;
    snap_nr_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 6, 22000.0, 51});
    snap_nr_in.track_occupants.push_back({6, 51});
    ta.on_occupancy(snap_nr_in, 22000.0);

    OccupancySnapshot snap_nr_out;
    snap_nr_out.count      = 0;
    snap_nr_out.confidence = OccupancyConfidence::Certain;
    snap_nr_out.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 6, 23000.0, 51});
    auto out_nr = ta.on_occupancy(snap_nr_out, 23000.0);
    CHECK(out_nr.completed_trips.size() == 1 &&
              (out_nr.completed_trips[0].flags & TrafficFlag::NO_RIDE) != 0 &&
              out_nr.completed_trips[0].confidence == "high",
          "same-floor entry and exit without ride sets no_ride while keeping high confidence");

    // Implausible load (> 15) on RunEnded -> NULL load_count + LOAD_IMPLAUSIBLE flag.
    auto out_impl = ta.on_timeline_events(
        {run_ev(TimelineEventType::RunEnded, "r99", "s1", 2, "s2", 5, "up", 25000.0, 35000.0)}, 275,
        OccupancyConfidence::Uncertain, 35000.0);
    CHECK(out_impl.completed_runs.size() == 1 && !out_impl.completed_runs[0].has_load_count &&
              (out_impl.completed_runs[0].flags & TrafficFlag::LOAD_IMPLAUSIBLE) != 0,
          "count above cabin capacity produces NULL load_count and load_implausible");
}

void test_timeouts_reconcile_cap_and_shutdown() {
    TripAssemblerConfig cfg;
    cfg.floor_tolerance_ms = 5000.0;
    cfg.max_trip_ms        = 60000.0;
    TripAssembler ta(cfg);

    auto s1 = make_stop("s1", 1, 1000.0, 1500.0, -1.0, -1.0, false);
    ta.on_timeline_events({stop_ev(TimelineEventType::StopOpened, s1, 1000.0)}, 0,
                          OccupancyConfidence::Certain, 1000.0);

    // Reconcile to 0 closes open trip with exit_unobserved.
    OccupancySnapshot s_in;
    s_in.count      = 1;
    s_in.confidence = OccupancyConfidence::Certain;
    s_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 1, 2000.0, 1});
    ta.on_occupancy(s_in, 2000.0);
    CHECK(ta.open_trip_count() == 1, "one open trip after Entry");

    OccupancySnapshot s_rec0;
    s_rec0.count      = 0;
    s_rec0.confidence = OccupancyConfidence::Certain;
    auto out_rec      = ta.on_occupancy(s_rec0, 3000.0);
    CHECK(out_rec.completed_trips.size() == 1 &&
              (out_rec.completed_trips[0].flags & TrafficFlag::EXIT_UNOBSERVED) != 0,
          "reconcile to 0 closes open trip as exit_unobserved");

    // 65 concurrent entries -> oldest evicted at 64 cap; remaining closed at max_trip_ms or
    // shutdown.
    for (int i = 1; i <= 65; ++i) {
        OccupancySnapshot snap;
        snap.count      = i;
        snap.confidence = OccupancyConfidence::Certain;
        snap.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, i, 10000.0 + i * 100.0,
                                             static_cast<std::uint64_t>(i + 10)});
        auto out = ta.on_occupancy(snap, 10000.0 + i * 100.0);
        if (i == 65) {
            CHECK(out.completed_trips.size() == 1 &&
                      (out.completed_trips[0].flags & TrafficFlag::EXIT_UNOBSERVED) != 0,
                  "exceeding 64 open trips evicts oldest with exit_unobserved");
        }
    }
    CHECK(ta.open_trip_count() == 64, "open_trips bounded at 64");

    // Advance past max_trip_ms for the earlier trips (say t=72000 closes trips entered <= 12000).
    auto out_to = ta.poll(72000.0);
    CHECK(!out_to.completed_trips.empty(), "poll closes trips exceeding max_trip_ms");

    auto out_sd = ta.shutdown(75000.0);
    CHECK(ta.open_trip_count() == 0 && !out_sd.completed_trips.empty(),
          "shutdown closes all remaining open trips with shutdown flag");
    bool all_sd = true;
    for (const auto& t : out_sd.completed_trips) {
        if ((t.flags & (TrafficFlag::SHUTDOWN | TrafficFlag::EXIT_UNOBSERVED)) !=
            (TrafficFlag::SHUTDOWN | TrafficFlag::EXIT_UNOBSERVED)) {
            all_sd = false;
        }
    }
    CHECK(all_sd, "all shutdown trips carry SHUTDOWN and EXIT_UNOBSERVED");
}

void test_visitor_samples_topk_and_crop_selection() {
    TripAssemblerConfig cfg;
    cfg.visitor_reid              = true;
    cfg.save_faces                = TrafficSaveFaces::Visitors;
    cfg.max_face_samples_per_trip = 3;
    TripAssembler ta(cfg);

    auto s1 = make_stop("s1", 1, 1000.0, 1500.0, -1.0, -1.0, false);
    ta.on_timeline_events({stop_ev(TimelineEventType::StopOpened, s1, 1000.0)}, 0,
                          OccupancyConfidence::Certain, 1000.0);

    OccupancySnapshot s_in;
    s_in.count      = 1;
    s_in.confidence = OccupancyConfidence::Certain;
    s_in.events.push_back(OccupancyEvent{OccupancyEventDirection::Entry, 7, 2000.0, 77});
    s_in.track_occupants.push_back({7, 77});
    ta.on_occupancy(s_in, 2000.0);

    ta.on_face_sample(7, make_sample(1.0f, 0.0f, 0.40f, 99, 0.25f, true), 2100.0);
    ta.on_face_sample(7, make_sample(0.9f, 0.1f, 0.85f, 99, 0.30f, true), 2200.0);
    ta.on_face_sample(7, make_sample(0.8f, 0.2f, 0.60f, -1, -1.0f, false), 2300.0);
    ta.on_face_sample(7, make_sample(0.7f, 0.3f, 0.20f, -1, -1.0f, false), 2400.0);
    CHECK(!ta.wants_crop_for_track(7, 0.50f), "skip cloning crop worse than best 0.85");
    CHECK(ta.wants_crop_for_track(7, 0.90f), "allow cloning crop better than 0.85");

    OccupancySnapshot s_out;
    s_out.count      = 0;
    s_out.confidence = OccupancyConfidence::Certain;
    s_out.events.push_back(OccupancyEvent{OccupancyEventDirection::Exit, 7, 3000.0, 77});
    auto out = ta.on_occupancy(s_out, 3000.0);
    CHECK(out.completed_trips.size() == 1, "visitor trip emitted");
    const auto& tr = out.completed_trips[0];
    CHECK(tr.identity_kind == "visitor" && tr.face_samples.size() == 3,
          "visitor trip retains top-3 samples sorted by quality");
    CHECK(tr.face_samples[0].quality == 0.85f && !tr.face_samples[0].crop_bgr_112.empty() &&
              tr.face_samples[2].crop_bgr_112.empty(),
          "only highest-quality sample retains 112x112 crop");
    CHECK(tr.suggested_resident_id == 99 && tr.suggested_resident_sim == 0.30f,
          "suggested resident preserved for below-threshold candidate");
}

} // namespace

int main() {
    test_normal_trip_pre_entry_identity_and_calls();
    test_delayed_stop_and_adjacent_tie_break();
    test_identity_conflict_no_ride_load_implausible_and_floor_unknown();
    test_timeouts_reconcile_cap_and_shutdown();
    test_visitor_samples_topk_and_crop_selection();
    std::printf("\n[test_trip_assembler] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
