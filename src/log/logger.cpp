#include "log/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <mutex>
#include <string>

#include <unistd.h>      // isatty, fileno

#if __has_include(<filesystem>)
#  include <filesystem>
namespace fs = std::filesystem;
#  define HAVE_FS 1
#else
#  define HAVE_FS 0
#endif

// --------------------------------------------------------------------------
// Impl: holds all mutable state + heavy std headers, out of the public header.
// --------------------------------------------------------------------------
struct Logger::Impl {
    LogConfig   cfg;
    std::mutex  mtx;
    FILE*       fp = nullptr;     // current day's file
    std::string cur_date;         // YYYY-MM-DD of fp
    bool        file_warned = false;
    bool        stderr_tty = false;
    bool        initialized = false;
};

Logger::Impl* Logger::impl() {
    if (!p_) p_ = new Impl();
    return p_;
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    shutdown();
    delete p_;
    p_ = nullptr;
}

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------
namespace {

// Split a system_clock time_point into local "YYYY-MM-DD" + "HH:MM:SS.mmm".
void format_time(std::chrono::system_clock::time_point tp,
                 std::string& date_out, char* time_buf, size_t time_buf_sz) {
    using namespace std::chrono;
    std::time_t t = system_clock::to_time_t(tp);
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char date_buf[40];
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                  tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday);
    date_out.assign(date_buf);
    std::snprintf(time_buf, time_buf_sz, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                  tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
                  static_cast<int>(ms.count()));
}

const char* color_for(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "\033[90m";   // bright black / grey
        case LogLevel::DEBUG: return "\033[36m";   // cyan
        case LogLevel::INFO:  return "\033[0m";    // default
        case LogLevel::WARN:  return "\033[33m";   // yellow
        case LogLevel::ERROR: return "\033[31m";   // red
        default:              return "\033[0m";
    }
}

// Parse "face-cabin-YYYY-MM-DD.log" -> days-since-epoch-ish key for comparison.
// Returns true and fills y/m/d if the name matches basename + date pattern.
bool parse_log_date(const std::string& fname, const std::string& basename,
                    int& y, int& m, int& d) {
    // Expect: <basename>-YYYY-MM-DD.log
    std::string prefix = basename + "-";
    const std::string suffix = ".log";
    if (fname.size() < prefix.size() + 10 + suffix.size()) return false;
    if (fname.compare(0, prefix.size(), prefix) != 0) return false;
    if (fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    std::string date = fname.substr(prefix.size(), 10);  // YYYY-MM-DD
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    y = std::atoi(date.substr(0, 4).c_str());
    m = std::atoi(date.substr(5, 2).c_str());
    d = std::atoi(date.substr(8, 2).c_str());
    return y > 1970 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// Very small "days since 1970" approximation for retention comparison. Good
// enough for "older than N days" — not a calendar-accurate diff.
long ymd_to_days(int y, int m, int d) {
    // Days from a fixed civil date algorithm (Howard Hinnant's days_from_civil).
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + static_cast<long>(doe) - 719468L;
}

}  // namespace

// --------------------------------------------------------------------------
// Level helpers
// --------------------------------------------------------------------------
LogLevel Logger::level_from_string(const std::string& s) {
    std::string t;
    for (char c : s) t += static_cast<char>(::tolower(c));
    if (t == "trace") return LogLevel::TRACE;
    if (t == "debug") return LogLevel::DEBUG;
    if (t == "info")  return LogLevel::INFO;
    if (t == "warn" || t == "warning") return LogLevel::WARN;
    if (t == "error" || t == "err")    return LogLevel::ERROR;
    if (t == "off" || t == "none")     return LogLevel::OFF;
    return LogLevel::INFO;
}

const char* Logger::level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default:              return "OFF  ";
    }
}

// --------------------------------------------------------------------------
// init / level
// --------------------------------------------------------------------------
void Logger::init(const LogConfig& cfg) {
    Impl* im = impl();
    std::lock_guard<std::mutex> lk(im->mtx);
    im->cfg = cfg;
    im->stderr_tty = ::isatty(fileno(stderr)) != 0;
    im->initialized = true;

    // Close any previously-open file (re-init).
    if (im->fp) { std::fclose(im->fp); im->fp = nullptr; im->cur_date.clear(); }

    if (!im->cfg.to_file) return;

#if HAVE_FS
    auto try_open_dir = [&](const std::string& dir) -> bool {
        std::error_code ec;
        fs::create_directories(dir, ec);   // ec ignored; open below is the real test
        return fs::exists(dir);
    };
    if (!try_open_dir(im->cfg.dir)) {
        std::fprintf(stderr,
            "[log] cannot create %s, falling back to ./logs\n",
            im->cfg.dir.c_str());
        im->cfg.dir = "./logs";
        if (!try_open_dir(im->cfg.dir)) {
            std::fprintf(stderr,
                "[log] cannot create ./logs either — file logging disabled\n");
            im->cfg.to_file = false;
        }
    }
#else
    // No <filesystem>: assume dir exists or is creatable by fopen path.
#endif
    // Actual file open happens lazily on first record (rotate).
}

void Logger::set_level(LogLevel lv) {
    Impl* im = impl();
    std::lock_guard<std::mutex> lk(im->mtx);
    im->cfg.level = lv;
}

LogLevel Logger::level() const {
    return p_ ? p_->cfg.level : LogLevel::INFO;
}

bool Logger::enabled(LogLevel lv) const {
    if (!p_) return lv >= LogLevel::INFO;   // pre-init: default INFO threshold
    if (p_->cfg.level == LogLevel::OFF) return false;
    return lv >= p_->cfg.level;
}

// --------------------------------------------------------------------------
// Rotation + cleanup (Task 1.3) — caller must hold mtx.
// --------------------------------------------------------------------------
// (declared here as a static free function operating on Impl to keep it simple)
namespace {

std::string file_path_for(const LogConfig& cfg, const std::string& date) {
    std::string p = cfg.dir;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    p += cfg.basename + "-" + date + ".log";
    return p;
}

void cleanup_old(const LogConfig& cfg, const std::string& today_date) {
#if HAVE_FS
    if (cfg.retention_days <= 0) return;
    int ty, tm, td;
    if (std::sscanf(today_date.c_str(), "%d-%d-%d", &ty, &tm, &td) != 3) return;
    long today_days = ymd_to_days(ty, tm, td);

    std::error_code ec;
    for (auto it = fs::directory_iterator(cfg.dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        int y, m, d;
        if (!parse_log_date(name, cfg.basename, y, m, d)) continue;
        long fdays = ymd_to_days(y, m, d);
        if (today_days - fdays > cfg.retention_days) {
            std::error_code rmec;
            fs::remove(it->path(), rmec);
        }
    }
#else
    (void)cfg; (void)today_date;
#endif
}

}  // namespace

// --------------------------------------------------------------------------
// log / vlog (Task 1.2)
// --------------------------------------------------------------------------
void Logger::log(LogLevel lv, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(lv, tag, fmt, ap);
    va_end(ap);
}

void Logger::vlog(LogLevel lv, const char* tag, const char* fmt, va_list ap) {
    Impl* im = impl();
    if (im->cfg.level == LogLevel::OFF || lv < im->cfg.level) return;

    // Format timestamp + message OUTSIDE the lock as much as possible.
    std::string date;
    char ts[40];
    format_time(std::chrono::system_clock::now(), date, ts, sizeof(ts));

    char msg_stack[1024];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = std::vsnprintf(msg_stack, sizeof(msg_stack), fmt, ap);
    std::string msg_dyn;
    const char* msg = msg_stack;
    if (n >= static_cast<int>(sizeof(msg_stack))) {
        msg_dyn.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(msg_dyn.data(), msg_dyn.size(), fmt, ap_copy);
        msg = msg_dyn.c_str();
    }
    va_end(ap_copy);

    const char* lname = level_name(lv);
    const char* t = tag ? tag : "-";

    std::lock_guard<std::mutex> lk(im->mtx);

    // ---- stderr sink ----
    if (im->cfg.to_stderr) {
        if (im->cfg.color && im->stderr_tty) {
            std::fprintf(stderr, "%s%s [%s] [%s] %s\033[0m\n",
                         color_for(lv), ts, lname, t, msg);
        } else {
            std::fprintf(stderr, "%s [%s] [%s] %s\n", ts, lname, t, msg);
        }
    }

    // ---- file sink (with daily rotation) ----
    if (im->cfg.to_file) {
        if (im->fp == nullptr || date != im->cur_date) {
            if (im->fp) { std::fclose(im->fp); im->fp = nullptr; }
            std::string path = file_path_for(im->cfg, date);
            im->fp = std::fopen(path.c_str(), "a");
            if (im->fp) {
                im->cur_date = date;
                cleanup_old(im->cfg, date);
            } else if (!im->file_warned) {
                std::fprintf(stderr,
                    "[log] cannot open %s — disabling file logging\n",
                    path.c_str());
                im->file_warned = true;
                im->cfg.to_file = false;
            }
        }
        if (im->fp) {
            std::fprintf(im->fp, "%s [%s] [%s] %s\n", ts, lname, t, msg);
            std::fflush(im->fp);   // durability > throughput (INFO+ is low volume)
        }
    }
}

void Logger::flush() {
    Impl* im = impl();
    std::lock_guard<std::mutex> lk(im->mtx);
    if (im->fp) std::fflush(im->fp);
}

void Logger::shutdown() {
    if (!p_) return;
    std::lock_guard<std::mutex> lk(p_->mtx);
    if (p_->fp) { std::fclose(p_->fp); p_->fp = nullptr; p_->cur_date.clear(); }
}

// --------------------------------------------------------------------------
// resolve_log_config (Task 2 lives in spec, but the free fn is declared in
// logger.h; provide it here so both app + tools can reuse it).
// Precedence: CLI > env > defaults.
// --------------------------------------------------------------------------
LogConfig resolve_log_config(int argc, char** argv, const LogConfig& defaults) {
    LogConfig cfg = defaults;

    if (const char* e = std::getenv("FACE_CABIN_LOG_LEVEL"))
        cfg.level = Logger::level_from_string(e);
    if (const char* e = std::getenv("FACE_CABIN_LOG_DIR")) {
        cfg.dir = e;
        cfg.to_file = true;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            cfg.level = Logger::level_from_string(argv[++i]);
        } else if (std::strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            cfg.dir = argv[++i];
            cfg.to_file = true;
        }
    }
    return cfg;
}
