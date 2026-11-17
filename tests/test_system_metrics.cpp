#include "system_metrics.h"

#include "log/logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

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

static void test_coherent_occupancy_snapshot() {
    SystemMetrics     metrics;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    metrics.set_occupancy(1, false);
    std::thread writer([&] {
        while (!start.load()) {
        }
        for (int i = 0; i < 500000; ++i) {
            metrics.set_occupancy(1, false);
            metrics.set_occupancy(2, true);
        }
        done.store(true);
    });
    start.store(true);
    while (!done.load()) {
        const SystemOccupancySnapshot snapshot = metrics.occupancy_snapshot();
        if (!snapshot.enabled || !((snapshot.count == 1 && !snapshot.uncertain) ||
                                   (snapshot.count == 2 && snapshot.uncertain))) {
            torn.store(true);
            break;
        }
    }
    writer.join();
    CHECK(!torn.load(), "occupancy count and confidence publish as one coherent snapshot");
}

int main() {
    test_coherent_occupancy_snapshot();

    FILE*     capture      = std::tmpfile();
    const int saved_stderr = dup(fileno(stderr));
    std::fflush(stderr);
    dup2(fileno(capture), fileno(stderr));
    LogConfig log_config;
    log_config.to_stderr = true;
    log_config.to_file   = false;
    log_config.color     = false;
    Logger::instance().init(log_config);

    SystemMetrics metrics(".", 1000);
    metrics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    metrics.set_occupancy(4, true);
    SystemTrafficSnapshot traffic;
    traffic.enabled              = true;
    traffic.queue_size           = 3;
    traffic.dropped              = 1;
    traffic.open_trips           = 2;
    traffic.open_calls           = 1;
    traffic.register_age_ms      = 420;
    traffic.register_invalid     = 5;
    traffic.run_counter_mismatch = 1;
    traffic.visitors_active      = 7;
    traffic.write_errors         = 2;
    metrics.set_traffic(traffic);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    metrics.stop();
    Logger::instance().flush();
    std::fflush(stderr);
    dup2(saved_stderr, fileno(stderr));
    close(saved_stderr);

    std::rewind(capture);
    std::string log;
    char        buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), capture))
        log += buffer;
    std::fclose(capture);
    Logger::instance().shutdown();

    bool               saw_disabled_sample = false;
    bool               saw_enabled_sample  = false;
    bool               saw_traffic_sample  = false;
    std::istringstream lines(log);
    std::string        line;
    while (std::getline(lines, line)) {
        if (line.find("threads=") == std::string::npos) continue;
        if (line.find("occupancy=") == std::string::npos) saw_disabled_sample = true;
        if (line.find("occupancy=4?") != std::string::npos) saw_enabled_sample = true;
        if (line.find("traffic_q=3") != std::string::npos &&
            line.find("visitors=7") != std::string::npos &&
            line.find("traffic_err=2") != std::string::npos) {
            saw_traffic_sample = true;
        }
    }
    CHECK(saw_disabled_sample, "system metrics omit occupancy until feature is enabled");
    CHECK(saw_enabled_sample, "system metrics include enabled occupancy state");
    CHECK(saw_traffic_sample, "system metrics include enabled traffic counters");
    std::printf("\n[test_system_metrics] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
