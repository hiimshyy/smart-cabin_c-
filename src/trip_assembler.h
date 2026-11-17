#pragma once

#include "app_config.h"
#include "call_tracker.h"
#include "elevator_registers.h"
#include "elevator_state.h"
#include "floor_call_coordinator.h"
#include "occupancy_counter.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct FaceSample {
    std::vector<float>        embedding; // 512-float L2-normalized
    float                     quality           = 0.0f;
    std::int64_t              best_resident_id  = -1;
    float                     best_resident_sim = -1.0f;
    std::vector<std::uint8_t> crop_bgr_112;
    int                       crop_w       = 0;
    int                       crop_h       = 0;
    double                    timestamp_ms = 0.0;
};

struct RunPassengerLink {
    std::string run_key;
    std::string trip_key;
};

struct PassengerTripRecord {
    std::string             trip_key;
    int                     cabin_id               = 2;
    double                  entered_at_ms          = -1.0;
    double                  exited_at_ms           = -1.0;
    int                     floor_in               = -1;
    bool                    has_floor_in           = false;
    int                     floor_out              = -1;
    bool                    has_floor_out          = false;
    std::string             identity_kind          = "unidentified";
    std::int64_t            resident_id            = -1;
    std::int64_t            visitor_id             = -1;
    float                   identity_sim           = -1.0f;
    std::int64_t            suggested_resident_id  = -1;
    float                   suggested_resident_sim = -1.0f;
    int                     called_floor           = -1;
    bool                    has_called_floor       = false;
    std::string             call_state;
    int                     call_register_confirmed = -1; // -1 = NULL, 0 = no, 1 = yes
    std::uint16_t           car_calls_mask          = 0;
    int                     rides                   = 0;
    std::string             boarded_stop_key;
    std::string             alighted_stop_key;
    std::string             hall_call_key;
    std::string             car_call_key;
    std::string             confidence = "high";
    std::uint32_t           flags      = 0;
    std::string             face_image;
    std::vector<FaceSample> face_samples;
};

struct TripAssemblerConfig {
    int              cabin_id                  = 2;
    int              slave                     = 2;
    bool             visitor_reid              = false;
    TrafficSaveFaces save_faces                = TrafficSaveFaces::Off;
    double           floor_tolerance_ms        = 10000.0;
    double           max_trip_ms               = 900000.0; // 15 min
    int              cabin_capacity            = 15;
    int              max_face_samples_per_trip = 5;
    double           pending_identity_ttl_ms   = 10000.0;
    float            suggest_min_sim           = 0.20f;
    std::string      session_hex               = "0000000000000001";
};

struct TripAssemblerOutput {
    std::vector<CabinStopRecord>     completed_stops;
    std::vector<CabinRunRecord>      completed_runs;
    std::vector<RunPassengerLink>    run_passengers;
    std::vector<PassengerTripRecord> completed_trips;
};

class TripAssembler {
public:
    explicit TripAssembler(TripAssemblerConfig config = {});

    TripAssemblerOutput on_timeline_events(const std::vector<TimelineEvent>& events,
                                           int                               current_occ_count,
                                           OccupancyConfidence               current_occ_confidence,
                                           double                            now_ms);

    TripAssemblerOutput on_occupancy(const OccupancySnapshot& snapshot, double now_ms);

    void on_resident_confirmed(int track_id, std::int64_t resident_id, float similarity,
                               double now_ms);

    // Caller can check this before cloning an aligned 112x112 face crop.
    bool wants_crop_for_track(int track_id, float quality) const;
    void on_face_sample(int track_id, FaceSample sample, double now_ms);

    void on_car_calls_mask(std::uint16_t current_mask, bool valid);
    void on_floor_call_decision(const FloorCallDecision& decision, double now_ms);
    void on_auto_call_confirmations(const std::vector<FloorCallConfirmEvent>& confirmations);
    void on_completed_calls(const std::vector<CarCallRecord>&  car_calls,
                            const std::vector<HallCallRecord>& hall_calls, double now_ms);

    TripAssemblerOutput poll(double now_ms);
    TripAssemblerOutput shutdown(double now_ms);

    std::size_t open_trip_count() const {
        return open_trips_.size();
    }

private:
    struct ResidentConfirmation {
        std::int64_t resident_id  = -1;
        float        similarity   = -1.0f;
        double       timestamp_ms = 0.0;
    };

    struct AutoCallLink {
        std::string  request_id;
        std::int64_t resident_id = -1;
        int          floor       = -1;
        std::string  state;
        int          register_confirmed = -1;
        double       timestamp_ms       = 0.0;
    };

    struct PendingIdentityBucket {
        int                               track_id        = -1;
        double                            last_updated_ms = 0.0;
        std::vector<ResidentConfirmation> confirmations;
        std::vector<FaceSample>           samples;
        std::int64_t                      suggested_resident_id  = -1;
        float                             suggested_resident_sim = -1.0f;
    };

    struct StopBufferEntry {
        CabinStopRecord record;
        std::string     next_run_direction;
    };

    struct ActiveTrip {
        PassengerTripRecord               record;
        bool                              entry_resolved        = false;
        double                            entry_timestamp_ms    = -1.0;
        bool                              exit_resolved         = false;
        double                            exit_timestamp_ms     = -1.0;
        double                            last_run_timestamp_ms = -1.0;
        bool                              had_gap_while_running = false;
        std::vector<ResidentConfirmation> confirmations;
        double                            active_since_ms = 0.0;
    };

    std::string next_trip_key();
    void        prune_pending(double now_ms);
    void        add_sample_to_list(std::vector<FaceSample>& list, FaceSample sample) const;
    float       best_crop_quality_in(const std::vector<FaceSample>& list) const;

    bool try_resolve_endpoint(double event_ms, bool is_entry, double now_ms, bool force_flush,
                              int& out_floor, bool& out_has_floor, std::string& out_stop_key,
                              std::uint32_t& out_flags);
    void resolve_waiting_trips(double now_ms, bool force_flush, TripAssemblerOutput& out);
    void flush_ready_stops(double now_ms, bool force_flush, TripAssemblerOutput& out);

    ActiveTrip close_open_trip(std::uint64_t occ_id, double exited_at_ms, std::uint32_t extra_flags,
                               bool exit_already_resolved);
    void       finalize_and_emit(ActiveTrip& trip, TripAssemblerOutput& out);
    void       apply_auto_call_links(ActiveTrip& trip) const;

    TripAssemblerConfig                  config_;
    std::uint64_t                        next_trip_seq_  = 1;
    bool                                 car_mask_valid_ = false;
    std::uint16_t                        prev_car_mask_  = 0;
    std::map<std::uint64_t, ActiveTrip>  open_trips_;
    std::vector<ActiveTrip>              awaiting_trips_;
    std::map<int, std::uint64_t>         track_to_occupant_;
    std::map<int, PendingIdentityBucket> pending_by_track_;
    std::deque<StopBufferEntry>          stop_buffer_;
    std::deque<CarCallRecord>            recent_car_calls_;
    std::deque<HallCallRecord>           recent_hall_calls_;
    std::deque<AutoCallLink>             recent_auto_calls_;
    std::string                          current_open_run_key_;
};
