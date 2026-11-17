#pragma once

#include "app_config.h"
#include "call_tracker.h"
#include "elevator_state.h"
#include "traffic_db.h"
#include "trip_assembler.h"
#include "visitor_gallery.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TrafficLogWriterConfig {
    std::string          db_path;
    std::string          schema_sql_path = "db/traffic_schema.sql";
    std::string          face_dir; // empty => <db_dir>/traffic_faces
    std::string          session_key             = "0000000000000001";
    std::int64_t         session_started_wall_ms = 0;
    int                  cabin_id                = 2;
    int                  elevator_slave          = 2;
    int                  recog_dim               = 512;
    bool                 visitor_reid            = false;
    TrafficSaveFaces     save_faces              = TrafficSaveFaces::Off;
    int                  traffic_retention_days  = 730;
    int                  visitor_retention_days  = 90;
    int                  face_retention_days     = 30;
    int                  min_free_mb             = 4096;
    int                  critical_free_mb        = 1024;
    std::uint64_t        face_max_total_bytes    = 5ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t          max_queue               = 1024;
    std::size_t          max_batch               = 50;
    double               disk_check_interval_ms  = 60000.0;
    double               purge_interval_ms       = 3600000.0;
    VisitorGalleryConfig gallery_config;

    using FreeSpaceFn   = std::function<bool(const std::string& path, std::uint64_t& free_bytes)>;
    using SteadyClockFn = std::function<double()>;
    using SystemClockFn = std::function<double()>;

    FreeSpaceFn   free_space_fn;
    SteadyClockFn steady_clock_fn;
    SystemClockFn system_clock_fn;
};

class TrafficLogWriter {
public:
    explicit TrafficLogWriter(TrafficLogWriterConfig config = {});
    ~TrafficLogWriter();

    TrafficLogWriter(const TrafficLogWriter&)            = delete;
    TrafficLogWriter& operator=(const TrafficLogWriter&) = delete;

    bool start();
    void close();

    void enqueue_assembler_output(const TripAssemblerOutput& out);
    void enqueue_calls(const CallStepResult& calls);
    void request_purge_now();

    bool is_running() const {
        return running_.load();
    }
    std::size_t   queue_size() const;
    std::uint64_t dropped_count() const {
        return dropped_count_.load();
    }
    std::uint64_t write_error_count() const {
        return write_error_count_.load();
    }
    std::uint64_t visitors_active() const {
        return visitors_active_.load();
    }
    std::uint64_t face_images_saved() const {
        return face_images_saved_.load();
    }

private:
    enum class ItemKind : std::uint8_t { Stop, Run, RunPassenger, Trip, CarCall, HallCall, Purge };

    struct WorkItem {
        ItemKind            kind = ItemKind::Stop;
        CabinStopRecord     stop;
        CabinRunRecord      run;
        RunPassengerLink    run_passenger;
        PassengerTripRecord trip;
        CarCallRecord       car_call;
        HallCallRecord      hall_call;
    };

    void        push_item_locked(WorkItem item);
    double      wall_offset_ms() const;
    void        worker_loop();
    void        refresh_disk_guard(double steady_now, bool force);
    void        run_purge(double steady_now);
    std::string maybe_save_face_jpeg(const PassengerTripRecord& trip);
    void        prune_face_files(std::int64_t wall_now_ms);

    TrafficLogWriterConfig config_;
    TrafficDB              db_;
    VisitorGallery         gallery_;

    mutable std::mutex      queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<WorkItem>    queue_;
    bool                    stop_requested_ = false;
    std::thread             worker_;
    std::atomic<bool>       running_{false};

    std::atomic<std::uint64_t> dropped_count_{0};
    std::atomic<std::uint64_t> write_error_count_{0};
    std::atomic<std::uint64_t> visitors_active_{0};
    std::atomic<std::uint64_t> face_images_saved_{0};

    double last_disk_check_ms_ = -1e12;
    double last_purge_ms_      = -1e12;
    double last_error_log_ms_  = -1e12;
    bool   allow_db_writes_    = true;
    bool   allow_face_saves_   = true;
};
