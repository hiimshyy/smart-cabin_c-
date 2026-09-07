// Unit test for the system logger (spec system-logging, Task 3).
// Dev-machine only (no NPU). Build via tests/run_tests.sh.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "log/logger.h"

static int g_checks = 0;
static int g_fail   = 0;
#define CHECK(cond, msg)                                             \
    do {                                                            \
        ++g_checks;                                                 \
        if (!(cond)) { ++g_fail;                                    \
            std::printf("  FAIL: %s  (line %d)\n", msg, __LINE__); }\
    } while (0)

static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) out.push_back(line);
    return out;
}

static std::string find_today_log(const std::string& dir,
                                  const std::string& basename) {
    // Build "<dir>/<basename>-<today>.log" the same way the logger does.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char date[16];
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return dir + "/" + basename + "-" + date + ".log";
}

int main() {
    const std::string dir = "/tmp/scabin_log_test";
    std::system(("rm -rf " + dir).c_str());

    // ---- Level filtering + format + file sink ----
    {
        LogConfig cfg;
        cfg.level     = LogLevel::WARN;   // DEBUG/INFO must be dropped
        cfg.to_stderr = false;            // keep test output clean
        cfg.to_file   = true;
        cfg.dir       = dir;
        cfg.basename  = "face-cabin";
        cfg.color     = false;
        Logger::instance().init(cfg);

        CHECK(!Logger::instance().enabled(LogLevel::INFO), "INFO disabled at WARN");
        CHECK(Logger::instance().enabled(LogLevel::WARN),  "WARN enabled at WARN");

        LOG_DEBUG("t", "debug should be dropped");
        LOG_INFO ("t", "info should be dropped");
        LOG_WARN ("cam", "warn kept %d", 1);
        LOG_ERROR("npu", "error kept");
        Logger::instance().flush();

        std::string path = find_today_log(dir, "face-cabin");
        auto lines = read_lines(path);
        CHECK(lines.size() == 2, "only 2 records (WARN+ERROR) written");

        std::regex re(
            R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[(TRACE|DEBUG|INFO |WARN |ERROR)\] \[.+\] .+$)");
        bool fmt_ok = !lines.empty();
        for (auto& l : lines) if (!std::regex_match(l, re)) fmt_ok = false;
        CHECK(fmt_ok, "log line format matches spec");

        bool has_warn = false, has_err = false;
        for (auto& l : lines) {
            if (l.find("[WARN ] [cam] warn kept 1") != std::string::npos) has_warn = true;
            if (l.find("[ERROR] [npu] error kept")  != std::string::npos) has_err = true;
        }
        CHECK(has_warn, "WARN line content correct");
        CHECK(has_err,  "ERROR line content correct");

        Logger::instance().shutdown();
    }

    // ---- Fallback: un-writable dir must not crash ----
    {
        LogConfig cfg;
        cfg.level     = LogLevel::INFO;
        cfg.to_stderr = false;
        cfg.to_file   = true;
        cfg.dir       = "/proc/should-not-be-writable/xyz"; // create should fail
        cfg.basename  = "face-cabin";
        Logger::instance().init(cfg);   // must fall back / disable, not crash
        LOG_INFO("t", "still alive after fallback");
        Logger::instance().flush();
        CHECK(true, "fallback dir did not crash");
        Logger::instance().shutdown();
    }

    // ---- Thread-safety: many threads, no interleaved/torn lines ----
    {
        const std::string tdir = "/tmp/scabin_log_test_mt";
        std::system(("rm -rf " + tdir).c_str());
        LogConfig cfg;
        cfg.level     = LogLevel::INFO;
        cfg.to_stderr = false;
        cfg.to_file   = true;
        cfg.dir       = tdir;
        cfg.basename  = "face-cabin";
        cfg.color     = false;
        Logger::instance().init(cfg);

        const int NT = 6, PER = 200;
        std::vector<std::thread> ths;
        for (int t = 0; t < NT; ++t) {
            ths.emplace_back([t]() {
                for (int i = 0; i < PER; ++i)
                    LOG_INFO("mt", "thread %d line %d payload=XYZ", t, i);
            });
        }
        for (auto& th : ths) th.join();
        Logger::instance().flush();

        std::string path = find_today_log(tdir, "face-cabin");
        auto lines = read_lines(path);
        CHECK((int)lines.size() == NT * PER, "all threads' lines present, none lost");
        std::regex re(R"(.*\[INFO \] \[mt\] thread \d+ line \d+ payload=XYZ$)");
        bool all_intact = !lines.empty();
        for (auto& l : lines) if (!std::regex_match(l, re)) all_intact = false;
        CHECK(all_intact, "no torn/interleaved lines");

        Logger::instance().shutdown();
        std::system(("rm -rf " + tdir).c_str());
    }

    std::system(("rm -rf " + dir).c_str());
    std::printf("\n[test_logger] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
