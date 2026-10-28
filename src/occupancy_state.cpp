#include "occupancy_state.h"

#include "json_minimal.h"
#include "log/logger.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace jm = json_minimal;

namespace {

constexpr auto                      SAVE_INTERVAL = std::chrono::seconds(1);
std::atomic<unsigned long>          temp_sequence{0};
std::atomic<AtomicSaveFailurePoint> save_failure_point{AtomicSaveFailurePoint::None};

double wall_clock_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(system_clock::now().time_since_epoch()).count();
}

std::string parent_directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool write_all(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

} // namespace

void set_occupancy_state_save_failure_for_test(AtomicSaveFailurePoint point) {
    save_failure_point.store(point);
}

OccupancyStateHolder::OccupancyStateHolder(std::string path) : path_(std::move(path)) {
    worker_ = std::thread(&OccupancyStateHolder::worker_loop, this);
}

OccupancyStateHolder::~OccupancyStateHolder() {
    close();
}

void OccupancyStateHolder::reset_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = OccupancyState{};
}

bool OccupancyStateHolder::load(std::string& error, OccupancyLoadStatus* status) {
    std::ifstream input(path_);
    if (!input) {
        reset_state();
        const bool missing = access(path_.c_str(), F_OK) != 0 && errno == ENOENT;
        error              = missing ? "state file is missing" : "state file cannot be read";
        if (status) {
            *status = missing ? OccupancyLoadStatus::Missing : OccupancyLoadStatus::Invalid;
        }
        LOG_WARN("occupancy", "%s; initialized count=0 confidence=uncertain",
                 missing ? "state file missing" : "state file invalid");
        return false;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    jm::JsonParser       parser(contents.str());
    jm::JsonValue        root;
    OccupancyState       loaded;
    int                  version = 0;
    std::string          confidence;
    const jm::JsonValue* value = nullptr;
    bool                 valid = parser.parse(root, error) && root.type == jm::JsonType::Object;
    if (valid) {
        value = jm::find(root, "version");
        valid = value && jm::as_int(*value, version) && version == 1;
        if (!valid) error = "missing/invalid state version";
    }
    if (valid) {
        value = jm::find(root, "count");
        valid = value && jm::as_int(*value, loaded.count) && loaded.count >= 0;
        if (!valid) error = "missing/invalid 'count'";
    }
    if (valid) {
        value = jm::find(root, "confidence");
        valid = value && jm::as_string(*value, confidence) &&
                (confidence == "certain" || confidence == "uncertain");
        if (!valid) error = "missing/invalid 'confidence'";
        loaded.confidence =
            confidence == "certain" ? OccupancyConfidence::Certain : OccupancyConfidence::Uncertain;
    }
    if (valid) {
        value = jm::find(root, "generation");
        valid = value && jm::as_int64(*value, loaded.generation) && loaded.generation >= 0;
        if (!valid) error = "missing/invalid 'generation'";
    }
    if (valid) {
        value = jm::find(root, "saved_at_ms");
        valid = value && value->type == jm::JsonType::Number && std::isfinite(value->number) &&
                value->number >= 0.0;
        if (!valid) {
            error = "missing/invalid 'saved_at_ms'";
        } else {
            loaded.saved_at_ms = value->number;
        }
    }

    if (!valid) {
        reset_state();
        if (status) *status = OccupancyLoadStatus::Invalid;
        LOG_WARN("occupancy", "state file invalid; initialized count=0 confidence=uncertain");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = loaded;
    }
    error.clear();
    if (status) *status = OccupancyLoadStatus::Loaded;
    return true;
}

bool OccupancyStateHolder::save_snapshot(const OccupancyState& state, std::string& error) const {
    jm::JsonValue root;
    root.type                      = jm::JsonType::Object;
    root.object["version"].type    = jm::JsonType::Number;
    root.object["version"].number  = 1;
    root.object["count"].type      = jm::JsonType::Number;
    root.object["count"].number    = state.count;
    root.object["confidence"].type = jm::JsonType::String;
    root.object["confidence"].string =
        state.confidence == OccupancyConfidence::Certain ? "certain" : "uncertain";
    root.object["generation"].type    = jm::JsonType::Number;
    root.object["generation"].number  = static_cast<double>(state.generation);
    root.object["saved_at_ms"].type   = jm::JsonType::Number;
    root.object["saved_at_ms"].number = state.saved_at_ms;

    std::ostringstream output;
    jm::serialize_value(output, root, 0);
    output << '\n';
    const std::string data = output.str();
    if (save_failure_point.load() == AtomicSaveFailurePoint::DirectoryOpen) {
        error = "cannot open state directory: injected failure";
        return false;
    }
    const int dir_fd = ::open(parent_directory(path_).c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        error = "cannot open state directory: " + std::string(std::strerror(errno));
        return false;
    }

    const std::string temp = path_ + ".tmp." + std::to_string(static_cast<long long>(getpid())) +
                             "." + std::to_string(temp_sequence.fetch_add(1));
    if (save_failure_point.load() == AtomicSaveFailurePoint::TempOpen) {
        error = "cannot open temporary state: injected failure";
        ::close(dir_fd);
        return false;
    }
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error = "cannot open temporary state: " + std::string(std::strerror(errno));
        ::close(dir_fd);
        return false;
    }
    bool ok = save_failure_point.load() != AtomicSaveFailurePoint::Write && write_all(fd, data);
    if (!ok) {
        error = save_failure_point.load() == AtomicSaveFailurePoint::Write
                    ? "cannot write temporary state: injected failure"
                    : "cannot write temporary state: " + std::string(std::strerror(errno));
    }
    if (ok && save_failure_point.load() == AtomicSaveFailurePoint::FileFsync) {
        ok    = false;
        error = "cannot fsync temporary state: injected failure";
    } else if (ok && fsync(fd) != 0) {
        ok    = false;
        error = "cannot fsync temporary state: " + std::string(std::strerror(errno));
    }
    if (::close(fd) != 0 && ok) {
        ok    = false;
        error = "cannot close temporary state: " + std::string(std::strerror(errno));
    }
    if (!ok) {
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }
    if (save_failure_point.load() == AtomicSaveFailurePoint::Rename) {
        error = "cannot rename temporary state: injected failure";
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }
    if (std::rename(temp.c_str(), path_.c_str()) != 0) {
        error = "cannot rename temporary state: " + std::string(std::strerror(errno));
        std::remove(temp.c_str());
        ::close(dir_fd);
        return false;
    }

    if (save_failure_point.load() == AtomicSaveFailurePoint::DirectoryFsync) {
        error = "cannot fsync state directory: injected failure";
        ::close(dir_fd);
        return false;
    }
    if (fsync(dir_fd) != 0) {
        error = "cannot fsync state directory: " + std::string(std::strerror(errno));
        ::close(dir_fd);
        return false;
    }
    ::close(dir_fd);
    return true;
}

bool OccupancyStateHolder::save(std::string& error) {
    return save_snapshot(get(), error);
}

void OccupancyStateHolder::set(int count, OccupancyConfidence confidence) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.count      = std::max(0, count);
    state_.confidence = confidence;
    ++state_.generation;
    state_.saved_at_ms = wall_clock_ms();
}

void OccupancyStateHolder::mark_uncertain(const char* /*reason*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.confidence == OccupancyConfidence::Uncertain) return;
    state_.confidence = OccupancyConfidence::Uncertain;
    ++state_.generation;
    state_.saved_at_ms = wall_clock_ms();
}

void OccupancyStateHolder::request_save(int count, OccupancyConfidence confidence) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || stopping_) return;
        const int normalized_count = std::max(0, count);
        if (state_.count != normalized_count || state_.confidence != confidence ||
            state_.saved_at_ms < 0.0) {
            state_.count      = normalized_count;
            state_.confidence = confidence;
            ++state_.generation;
            state_.saved_at_ms = wall_clock_ms();
        }
        pending_ = state_;
    }
    cv_.notify_one();
}

OccupancyState OccupancyStateHolder::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void OccupancyStateHolder::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto                         next_save = std::chrono::steady_clock::time_point::min();
    while (true) {
        cv_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
        if (stopping_ && !pending_) break;
        if (!pending_) continue;

        const auto now = std::chrono::steady_clock::now();
        if (next_save > now) {
            cv_.wait_until(lock, next_save);
            continue;
        }
        const OccupancyState snapshot = *pending_;
        pending_.reset();
        lock.unlock();
        std::string error;
        if (!save_snapshot(snapshot, error)) {
            LOG_WARN("occupancy", "state persistence failed: %s", error.c_str());
        }
        next_save = std::chrono::steady_clock::now() + SAVE_INTERVAL;
        lock.lock();
    }
}

void OccupancyStateHolder::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}
