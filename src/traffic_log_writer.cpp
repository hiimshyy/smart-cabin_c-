#include "traffic_log_writer.h"

#include "log/logger.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

namespace {

double default_steady_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

double default_system_ms() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

bool default_free_space(const std::string& path, std::uint64_t& free_bytes) {
    struct statvfs st {};
    if (::statvfs(path.c_str(), &st) != 0) return false;
    free_bytes = static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_frsize);
    return true;
}

std::string parent_dir_or_dot(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool ensure_dir_recursive(const std::string& dir) {
    if (dir.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (cur == "/") continue;
            if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

double shift_ms(double steady_ms, double offset_ms) {
    return (steady_ms >= 0.0 && std::isfinite(steady_ms)) ? (steady_ms + offset_ms) : steady_ms;
}

std::string format_utc_date(std::int64_t wall_ms) {
    const std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
    std::tm           tm_buf{};
    gmtime_r(&secs, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                  tm_buf.tm_mday);
    return std::string(buf);
}

struct FaceFileInfo {
    std::string   path;
    std::time_t   mtime = 0;
    std::uint64_t size  = 0;
};

void collect_face_files(const std::string& dir, std::vector<FaceFileInfo>& files) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        struct stat       st {};
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_face_files(full, files);
        } else if (S_ISREG(st.st_mode)) {
            files.push_back(
                FaceFileInfo{full, st.st_mtime, static_cast<std::uint64_t>(st.st_size)});
        }
    }
    ::closedir(d);
}

void remove_empty_subdirs(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        struct stat       st {};
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            ::rmdir(full.c_str()); // only succeeds when empty
        }
    }
    ::closedir(d);
}

} // namespace

TrafficLogWriter::TrafficLogWriter(TrafficLogWriterConfig config)
    : config_(std::move(config)), gallery_([&] {
          VisitorGalleryConfig gc = config_.gallery_config;
          gc.dim                  = config_.recog_dim;
          return gc;
      }()) {
    if (config_.face_dir.empty() && !config_.db_path.empty()) {
        config_.face_dir = parent_dir_or_dot(config_.db_path) + "/traffic_faces";
    }
    if (!config_.free_space_fn) config_.free_space_fn = default_free_space;
    if (!config_.steady_clock_fn) config_.steady_clock_fn = default_steady_ms;
    if (!config_.system_clock_fn) config_.system_clock_fn = default_system_ms;
}

TrafficLogWriter::~TrafficLogWriter() {
    close();
}

bool TrafficLogWriter::start() {
    if (running_.load()) return true;
    if (config_.db_path.empty()) {
        LOG_ERROR("traffic", "empty traffic DB path; module disabled");
        return false;
    }

    if (!db_.open(config_.db_path, config_.schema_sql_path)) {
        LOG_ERROR("traffic", "failed to open/migrate traffic DB; module disabled");
        return false;
    }

    const std::int64_t start_wall =
        config_.session_started_wall_ms > 0
            ? config_.session_started_wall_ms
            : static_cast<std::int64_t>(std::llround(config_.system_clock_fn()));
    std::string err;
    if (!db_.insert_session(config_.session_key, start_wall, config_.cabin_id,
                            config_.elevator_slave, &err)) {
        LOG_ERROR("traffic", "failed to write app_session: %s; module disabled", err.c_str());
        db_.close();
        return false;
    }

    if (config_.visitor_reid) {
        std::vector<VisitorRecord> active;
        if (!db_.load_active_visitors(active, config_.recog_dim)) {
            LOG_WARN("traffic", "failed to load active visitors at startup");
        } else {
            gallery_.load(active);
        }
    }
    visitors_active_.store(static_cast<std::uint64_t>(gallery_.visitor_count()));

    const double steady_now = config_.steady_clock_fn();
    last_disk_check_ms_     = -1e12;
    last_purge_ms_          = steady_now;
    stop_requested_         = false;
    running_.store(true);
    worker_ = std::thread(&TrafficLogWriter::worker_loop, this);
    return true;
}

void TrafficLogWriter::close() {
    if (!running_.load() && !worker_.joinable()) {
        db_.close();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        stop_requested_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    db_.close();
}

std::size_t TrafficLogWriter::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    return queue_.size();
}

double TrafficLogWriter::wall_offset_ms() const {
    return config_.system_clock_fn() - config_.steady_clock_fn();
}

void TrafficLogWriter::push_item_locked(WorkItem item) {
    const std::size_t cap = std::max<std::size_t>(1, config_.max_queue);
    if (queue_.size() >= cap) {
        queue_.pop_front();
        dropped_count_.fetch_add(1);
    }
    queue_.push_back(std::move(item));
}

void TrafficLogWriter::enqueue_assembler_output(const TripAssemblerOutput& out) {
    if (!running_.load()) return;
    if (out.completed_stops.empty() && out.completed_runs.empty() && out.run_passengers.empty() &&
        out.completed_trips.empty()) {
        return;
    }
    const double offset = wall_offset_ms();
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        for (const auto& s : out.completed_stops) {
            WorkItem w;
            w.kind                   = ItemKind::Stop;
            w.stop                   = s;
            w.stop.arrived_at_ms     = shift_ms(s.arrived_at_ms, offset);
            w.stop.door_opened_at_ms = shift_ms(s.door_opened_at_ms, offset);
            w.stop.door_closed_at_ms = shift_ms(s.door_closed_at_ms, offset);
            w.stop.departed_at_ms    = shift_ms(s.departed_at_ms, offset);
            push_item_locked(std::move(w));
        }
        for (const auto& r : out.completed_runs) {
            WorkItem w;
            w.kind               = ItemKind::Run;
            w.run                = r;
            w.run.departed_at_ms = shift_ms(r.departed_at_ms, offset);
            w.run.arrived_at_ms  = shift_ms(r.arrived_at_ms, offset);
            push_item_locked(std::move(w));
        }
        for (const auto& rp : out.run_passengers) {
            WorkItem w;
            w.kind          = ItemKind::RunPassenger;
            w.run_passenger = rp;
            push_item_locked(std::move(w));
        }
        for (const auto& t : out.completed_trips) {
            WorkItem w;
            w.kind               = ItemKind::Trip;
            w.trip               = t;
            w.trip.entered_at_ms = shift_ms(t.entered_at_ms, offset);
            w.trip.exited_at_ms  = shift_ms(t.exited_at_ms, offset);
            push_item_locked(std::move(w));
        }
    }
    queue_cv_.notify_one();
}

void TrafficLogWriter::enqueue_calls(const CallStepResult& calls) {
    if (!running_.load()) return;
    if (calls.completed_car_calls.empty() && calls.completed_hall_calls.empty()) return;
    const double offset = wall_offset_ms();
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        for (const auto& cc : calls.completed_car_calls) {
            WorkItem w;
            w.kind                      = ItemKind::CarCall;
            w.car_call                  = cc;
            w.car_call.registered_at_ms = shift_ms(cc.registered_at_ms, offset);
            w.car_call.cleared_at_ms    = shift_ms(cc.cleared_at_ms, offset);
            push_item_locked(std::move(w));
        }
        for (const auto& hc : calls.completed_hall_calls) {
            WorkItem w;
            w.kind                        = ItemKind::HallCall;
            w.hall_call                   = hc;
            w.hall_call.registered_at_ms  = shift_ms(hc.registered_at_ms, offset);
            w.hall_call.cleared_at_ms     = shift_ms(hc.cleared_at_ms, offset);
            w.hall_call.door_opened_at_ms = shift_ms(hc.door_opened_at_ms, offset);
            push_item_locked(std::move(w));
        }
    }
    queue_cv_.notify_one();
}

void TrafficLogWriter::request_purge_now() {
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        WorkItem                    w;
        w.kind = ItemKind::Purge;
        push_item_locked(std::move(w));
    }
    queue_cv_.notify_one();
}

void TrafficLogWriter::refresh_disk_guard(double steady_now, bool force) {
    if (!force && (steady_now - last_disk_check_ms_ < config_.disk_check_interval_ms)) return;
    last_disk_check_ms_ = steady_now;

    const std::string db_dir     = parent_dir_or_dot(config_.db_path);
    std::uint64_t     free_bytes = 0;
    if (!config_.free_space_fn(db_dir, free_bytes)) return;

    if (config_.save_faces != TrafficSaveFaces::Off && !config_.face_dir.empty()) {
        std::uint64_t face_free = 0;
        if (config_.free_space_fn(config_.face_dir, face_free)) {
            free_bytes = std::min(free_bytes, face_free);
        }
    }

    const std::uint64_t free_mb = free_bytes / (1024ULL * 1024ULL);
    const bool          prev_db = allow_db_writes_;
    allow_db_writes_            = (free_mb >= static_cast<std::uint64_t>(config_.critical_free_mb));
    allow_face_saves_ =
        allow_db_writes_ && (free_mb >= static_cast<std::uint64_t>(config_.min_free_mb));

    if (!allow_db_writes_ && (prev_db || steady_now - last_error_log_ms_ >= 60000.0)) {
        LOG_ERROR("traffic", "disk critically low free_mb=%llu (< %d); pausing traffic DB writes",
                  (unsigned long long)free_mb, config_.critical_free_mb);
        last_error_log_ms_ = steady_now;
    } else if (!allow_face_saves_ && config_.save_faces != TrafficSaveFaces::Off &&
               steady_now - last_error_log_ms_ >= 60000.0) {
        LOG_WARN("traffic", "disk low free_mb=%llu (< %d); pausing face image writes",
                 (unsigned long long)free_mb, config_.min_free_mb);
        last_error_log_ms_ = steady_now;
    }
}

std::string TrafficLogWriter::maybe_save_face_jpeg(const PassengerTripRecord& trip) {
    if (config_.save_faces == TrafficSaveFaces::Off || !allow_face_saves_ ||
        config_.face_dir.empty()) {
        return {};
    }
    if (config_.save_faces == TrafficSaveFaces::Visitors && trip.identity_kind == "resident") {
        return {};
    }

    const FaceSample* best = nullptr;
    for (const auto& s : trip.face_samples) {
        if (!s.crop_bgr_112.empty() && s.crop_w > 0 && s.crop_h > 0 &&
            (!best || s.quality > best->quality)) {
            best = &s;
        }
    }
    if (!best) return {};

    const std::size_t expected_bytes =
        static_cast<std::size_t>(best->crop_w) * static_cast<std::size_t>(best->crop_h) * 3u;
    if (best->crop_bgr_112.size() != expected_bytes) return {};

    const double       ts_ms = trip.entered_at_ms >= 0.0 ? trip.entered_at_ms : trip.exited_at_ms;
    const std::int64_t wall_ms =
        ts_ms >= 0.0 ? static_cast<std::int64_t>(std::llround(ts_ms))
                     : static_cast<std::int64_t>(std::llround(config_.system_clock_fn()));
    const std::string sub_dir = config_.face_dir + "/" + format_utc_date(wall_ms);
    if (!ensure_dir_recursive(sub_dir)) return {};

    const std::string out_path = sub_dir + "/" + trip.trip_key + ".jpg";
    cv::Mat           img(best->crop_h, best->crop_w, CV_8UC3,
                          const_cast<std::uint8_t*>(best->crop_bgr_112.data()));
    std::vector<int>  params = {cv::IMWRITE_JPEG_QUALITY, 90};
    if (!cv::imwrite(out_path, img, params)) return {};
    ::chmod(out_path.c_str(), 0600);
    face_images_saved_.fetch_add(1);
    return out_path;
}

void TrafficLogWriter::prune_face_files(std::int64_t wall_now_ms) {
    if (config_.face_dir.empty()) return;
    std::vector<FaceFileInfo> files;
    collect_face_files(config_.face_dir, files);
    if (files.empty()) return;

    const std::time_t now_secs = static_cast<std::time_t>(wall_now_ms / 1000);
    const std::time_t max_age_sec =
        static_cast<std::time_t>(std::max(1, config_.face_retention_days)) * 86400;

    std::vector<FaceFileInfo> kept;
    std::uint64_t             total_bytes = 0;
    for (const auto& f : files) {
        if (now_secs - f.mtime > max_age_sec) {
            ::unlink(f.path.c_str());
        } else {
            total_bytes += f.size;
            kept.push_back(f);
        }
    }

    if (total_bytes > config_.face_max_total_bytes) {
        std::sort(kept.begin(), kept.end(),
                  [](const FaceFileInfo& a, const FaceFileInfo& b) { return a.mtime < b.mtime; });
        for (const auto& f : kept) {
            if (total_bytes <= config_.face_max_total_bytes) break;
            if (::unlink(f.path.c_str()) == 0) {
                total_bytes -= std::min(total_bytes, f.size);
            }
        }
    }
    remove_empty_subdirs(config_.face_dir);
}

void TrafficLogWriter::run_purge(double steady_now) {
    last_purge_ms_ = steady_now;
    const std::int64_t wall_now =
        static_cast<std::int64_t>(std::llround(config_.system_clock_fn()));
    TrafficPurgeResult purge_res;
    std::string        err;
    if (!db_.purge_expired(wall_now, config_.traffic_retention_days, config_.visitor_retention_days,
                           purge_res, &err)) {
        write_error_count_.fetch_add(1);
        if (steady_now - last_error_log_ms_ >= 5000.0) {
            LOG_WARN("traffic", "hourly purge failed error=%s", err.c_str());
            last_error_log_ms_ = steady_now;
        }
    } else if (purge_res.rows_deleted > 0 || purge_res.visitors_expired > 0) {
        LOG_INFO("traffic", "purge completed deleted=%lld expired_visitors=%lld",
                 (long long)purge_res.rows_deleted, (long long)purge_res.visitors_expired);
        if (config_.visitor_reid && purge_res.visitors_expired > 0) {
            std::vector<VisitorRecord> active;
            if (db_.load_active_visitors(active, config_.recog_dim)) {
                gallery_.load(active);
                visitors_active_.store(static_cast<std::uint64_t>(gallery_.visitor_count()));
            }
        }
    }
    prune_face_files(wall_now);
}

void TrafficLogWriter::worker_loop() {
    using namespace std::chrono_literals;
    std::vector<WorkItem> batch;
    batch.reserve(config_.max_batch);

    while (true) {
        bool purge_requested = false;
        batch.clear();
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, 500ms, [&] { return stop_requested_ || !queue_.empty(); });
            if (queue_.empty() && stop_requested_) break;
            const std::size_t limit = std::max<std::size_t>(1, config_.max_batch);
            while (!queue_.empty() && batch.size() < limit) {
                WorkItem front = std::move(queue_.front());
                queue_.pop_front();
                if (front.kind == ItemKind::Purge) {
                    purge_requested = true;
                } else {
                    batch.push_back(std::move(front));
                }
            }
        }

        const double steady_now = config_.steady_clock_fn();
        refresh_disk_guard(steady_now, false);

        if (!batch.empty() && allow_db_writes_) {
            TrafficWriteBatch wb;
            for (auto& item : batch) {
                switch (item.kind) {
                case ItemKind::Stop:
                    wb.stops.push_back(std::move(item.stop));
                    break;
                case ItemKind::Run:
                    wb.runs.push_back(std::move(item.run));
                    break;
                case ItemKind::RunPassenger:
                    wb.run_passengers.push_back(std::move(item.run_passenger));
                    break;
                case ItemKind::CarCall:
                    wb.car_calls.push_back(std::move(item.car_call));
                    break;
                case ItemKind::HallCall:
                    wb.hall_calls.push_back(std::move(item.hall_call));
                    break;
                case ItemKind::Trip: {
                    if (config_.visitor_reid && item.trip.identity_kind == "visitor") {
                        const double       ref_ms = item.trip.entered_at_ms >= 0.0
                                                        ? item.trip.entered_at_ms
                                                        : item.trip.exited_at_ms;
                        const std::int64_t seen_ms =
                            ref_ms >= 0.0 ? static_cast<std::int64_t>(std::llround(ref_ms))
                                          : static_cast<std::int64_t>(
                                                std::llround(config_.system_clock_fn()));
                        const VisitorResolveResult vr =
                            gallery_.resolve(item.trip.face_samples, seen_ms, &db_);
                        if (vr.resolved) {
                            item.trip.identity_kind = "visitor";
                            item.trip.visitor_id    = vr.visitor_id;
                            item.trip.identity_sim  = vr.similarity;
                            visitors_active_.store(
                                static_cast<std::uint64_t>(gallery_.visitor_count()));
                        } else {
                            item.trip.identity_kind = "unidentified";
                            item.trip.visitor_id    = -1;
                        }
                    }
                    item.trip.face_image = maybe_save_face_jpeg(item.trip);
                    LOG_INFO("traffic",
                             "TRIP key=%s kind=%s resident_id=%lld visitor_id=%lld floor_in=%d "
                             "floor_out=%d conf=%s flags=%u",
                             item.trip.trip_key.c_str(), item.trip.identity_kind.c_str(),
                             (long long)item.trip.resident_id, (long long)item.trip.visitor_id,
                             item.trip.has_floor_in ? item.trip.floor_in : -1,
                             item.trip.has_floor_out ? item.trip.floor_out : -1,
                             item.trip.confidence.c_str(), item.trip.flags);
                    wb.trips.push_back(std::move(item.trip));
                    break;
                }
                case ItemKind::Purge:
                    break;
                }
            }

            std::string err;
            if (!db_.insert_batch(wb, &err)) {
                write_error_count_.fetch_add(1);
                if (steady_now - last_error_log_ms_ >= 5000.0) {
                    LOG_WARN("traffic", "batch insert failed error=%s", err.c_str());
                    last_error_log_ms_ = steady_now;
                }
            }
        }

        if (purge_requested || (steady_now - last_purge_ms_ >= config_.purge_interval_ms)) {
            run_purge(steady_now);
        }
    }
}
