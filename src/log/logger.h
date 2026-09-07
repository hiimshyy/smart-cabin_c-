#pragma once
// Application system/operational logger (spec system-logging, Task 1).
//
// Self-written, no external dependency (C++17 std + POSIX). Provides levels,
// thread-safe writes, stderr + daily-rotated file sinks, and retention cleanup.
//
// NOTE: This is NOT src/log/log.h — that file is a shim that MUTES the NPU
// SDK (awnn) verbose logs and must stay untouched. This logger is for the
// application itself. Include it as:  #include "log/logger.h"
//
// Usage:
//   LogConfig cfg;                    // or resolve_log_config(argc, argv, ...)
//   cfg.to_file = true;
//   Logger::instance().init(cfg);
//   LOG_INFO("cam", "opened /dev/video%d %dx%d", id, w, h);
//
// Privacy (R7): never log resident names / greeting_name / apartment. Use
// numeric resident_id / track_id instead.

#include <cstdarg>
#include <string>

enum class LogLevel { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, OFF = 5 };

struct LogConfig {
    LogLevel    level          = LogLevel::INFO;
    bool        to_stderr      = true;
    bool        to_file        = false;              // main app on; tools off
    std::string dir            = "/var/log/face-cabin";
    std::string basename       = "face-cabin";       // -> face-cabin-YYYY-MM-DD.log
    int         retention_days = 14;
    bool        color          = true;               // only if stderr is a TTY
};

class Logger {
public:
    static Logger& instance();

    // Initialize once near the start of main(). If to_file cannot open `dir`,
    // falls back to ./logs, then to stderr-only — never throws / crashes.
    void init(const LogConfig& cfg);

    void     set_level(LogLevel lv);
    LogLevel level() const;
    bool     enabled(LogLevel lv) const;   // cheap check for the LOG_ macros

    // Emit one record (macros call these after an enabled() check).
    void log(LogLevel lv, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlog(LogLevel lv, const char* tag, const char* fmt, va_list ap);

    void flush();
    void shutdown();   // close file; safe to call multiple times

    // Config helpers.
    static LogLevel    level_from_string(const std::string& s);  // "info" -> INFO
    static const char* level_name(LogLevel lv);                  // INFO -> "INFO "

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();
    struct Impl;
    Impl* p_ = nullptr;   // pimpl to keep <mutex>/<cstdio>/<filesystem> out of header
    Impl* impl();
};

// ---- Macros: check level BEFORE formatting so filtered records are cheap ----
#define LOG_AT(lv, tag, ...)                                       \
    do {                                                          \
        ::Logger& _lg = ::Logger::instance();                     \
        if (_lg.enabled(lv)) _lg.log((lv), (tag), __VA_ARGS__);   \
    } while (0)

#define LOG_TRACE(tag, ...) LOG_AT(::LogLevel::TRACE, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) LOG_AT(::LogLevel::DEBUG, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  LOG_AT(::LogLevel::INFO,  tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  LOG_AT(::LogLevel::WARN,  tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) LOG_AT(::LogLevel::ERROR, tag, __VA_ARGS__)

// ---- Config resolution: CLI > env > defaults (R5.5) ----
// Scans argv for --log-level / --log-dir; applies FACE_CABIN_LOG_LEVEL /
// FACE_CABIN_LOG_DIR env first. Passing --log-dir implicitly enables to_file.
// Does not consume/modify argv (leaves it for the caller's own parser).
LogConfig resolve_log_config(int argc, char** argv, const LogConfig& defaults);
