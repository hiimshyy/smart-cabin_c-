#include "system_metrics.h"

#include "log/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct CpuSample {
    uint64_t total = 0;
    uint64_t idle = 0;
    uint64_t process = 0;
    bool valid = false;
};

struct HostSample {
    CpuSample cpu;
    double uptime_s = -1.0;
    double load1 = -1.0;
    double load5 = -1.0;
    double load15 = -1.0;
    double ram_used_pct = -1.0;
    uint64_t ram_total_mb = 0;
    uint64_t ram_available_mb = 0;
    uint64_t rss_mb = 0;
    int threads = -1;
    int fd_count = -1;
    double disk_used_pct = -1.0;
    double disk_free_gb = -1.0;
    double cpu_temp_c = -1.0;
    double gpu_temp_c = -1.0;
    double npu_temp_c = -1.0;
    double ddr_temp_c = -1.0;
    double skin_temp_c = -1.0;
};

bool read_uptime(double& value) {
    std::ifstream in("/proc/uptime");
    return static_cast<bool>(in >> value);
}

bool read_cpu(CpuSample& out) {
    std::ifstream in("/proc/stat");
    std::string label;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0, softirq = 0, steal = 0;
    if (!(in >> label >> user >> nice >> system >> idle >> iowait
          >> irq >> softirq >> steal) || label != "cpu") return false;

    out.total = user + nice + system + idle + iowait + irq + softirq + steal;
    out.idle = idle + iowait;

    std::ifstream self("/proc/self/stat");
    std::string line;
    if (!std::getline(self, line)) return false;
    const std::size_t comm_end = line.rfind(')');
    if (comm_end == std::string::npos) return false;
    std::istringstream fields(line.substr(comm_end + 1));
    std::string state;
    uint64_t ignored = 0, utime = 0, stime = 0;
    if (!(fields >> state)) return false;
    // After the comm field: state is field 3, utime is field 14, stime 15.
    for (int field = 4; field <= 13; ++field) {
        if (!(fields >> ignored)) return false;
    }
    if (!(fields >> utime >> stime)) return false;
    out.process = utime + stime;
    out.valid = true;
    return true;
}

bool read_memory(HostSample& out) {
    std::ifstream in("/proc/meminfo");
    uint64_t total_kb = 0, available_kb = 0;
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (in >> key >> value >> unit) {
        if (key == "MemTotal:") total_kb = value;
        else if (key == "MemAvailable:") available_kb = value;
    }
    if (total_kb == 0) return false;
    out.ram_total_mb = total_kb / 1024;
    out.ram_available_mb = available_kb / 1024;
    out.ram_used_pct = 100.0 * static_cast<double>(total_kb - std::min(total_kb, available_kb))
                       / static_cast<double>(total_kb);

    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields(line);
        if (!(fields >> key >> value)) continue;
        if (key == "VmRSS:") out.rss_mb = value / 1024;
        else if (key == "Threads:") out.threads = static_cast<int>(value);
    }
    return true;
}

int count_fds() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    while (readdir(dir) != nullptr) ++count;
    closedir(dir);
    // . and .. are included.
    return std::max(0, count - 2);
}

bool read_storage(const std::string& path, HostSample& out) {
    struct statvfs stat{};
    if (statvfs(path.c_str(), &stat) != 0 || stat.f_blocks == 0) return false;
    const double total = static_cast<double>(stat.f_blocks) * stat.f_frsize;
    const double available = static_cast<double>(stat.f_bavail) * stat.f_frsize;
    out.disk_free_gb = available / (1024.0 * 1024.0 * 1024.0);
    out.disk_used_pct = 100.0 * (1.0 - available / total);
    return true;
}

bool read_temperature(HostSample& out) {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return false;
    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.rfind("thermal_zone", 0) != 0) continue;
        const std::string base = "/sys/class/thermal/" + name;
        std::ifstream temp(base + "/temp");
        long millidegrees = 0;
        if (!(temp >> millidegrees)) continue;
        const double celsius = static_cast<double>(millidegrees) / 1000.0;
        std::ifstream type_file(base + "/type");
        std::string type;
        std::getline(type_file, type);
        if (type == "cpul_thermal_zone" || type == "cpub_thermal_zone")
            out.cpu_temp_c = std::max(out.cpu_temp_c, celsius);
        else if (type == "gpu_thermal_zone")
            out.gpu_temp_c = std::max(out.gpu_temp_c, celsius);
        else if (type == "npu_thermal_zone")
            out.npu_temp_c = std::max(out.npu_temp_c, celsius);
        else if (type == "ddr_thermal_zone")
            out.ddr_temp_c = std::max(out.ddr_temp_c, celsius);
        else if (type == "skin_zone")
            out.skin_temp_c = std::max(out.skin_temp_c, celsius);
    }
    closedir(dir);
    return out.cpu_temp_c >= -50.0 || out.gpu_temp_c >= -50.0 ||
           out.npu_temp_c >= -50.0 || out.ddr_temp_c >= -50.0 ||
           out.skin_temp_c >= -50.0;
}

bool read_host(const std::string& storage_path, HostSample& out) {
    bool ok = true;
    ok = read_cpu(out.cpu) && ok;
    ok = read_uptime(out.uptime_s) && ok;
    double load_values[3] = {};
    if (getloadavg(load_values, 3) == 3) {
        out.load1 = load_values[0];
        out.load5 = load_values[1];
        out.load15 = load_values[2];
    } else {
        ok = false;
    }
    ok = read_memory(out) && ok;
    out.fd_count = count_fds();
    ok = out.fd_count >= 0 && ok;
    ok = read_storage(storage_path, out) && ok;
    ok = read_temperature(out) && ok;
    return ok;
}

double percent_delta(uint64_t current, uint64_t previous,
                     uint64_t total_current, uint64_t total_previous) {
    if (total_current <= total_previous) return -1.0;
    const uint64_t delta_total = total_current - total_previous;
    if (current < previous) return -1.0;
    return 100.0 * static_cast<double>(current - previous)
           / static_cast<double>(delta_total);
}

}  // namespace

SystemMetrics::SystemMetrics(std::string storage_path, int interval_ms)
    : storage_path_(std::move(storage_path)), interval_ms_(std::max(1000, interval_ms)) {}

SystemMetrics::~SystemMetrics() {
    stop();
}

void SystemMetrics::start() {
    if (worker_.joinable()) return;
    stop_requested_.store(false);
    worker_ = std::thread(&SystemMetrics::worker_loop, this);
}

void SystemMetrics::stop() {
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SystemMetrics::worker_loop() {
    HostSample previous;
    bool have_previous = read_host(storage_path_, previous);
    const auto started = std::chrono::steady_clock::now();
    auto last_warning = started - std::chrono::minutes(1);

    LOG_INFO("sys", "monitor started interval_ms=%d storage=%s", interval_ms_, storage_path_.c_str());

    while (!stop_requested_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(interval_ms_),
                         [this] { return stop_requested_.load(); })) break;
        lock.unlock();

        HostSample current;
        const bool complete = read_host(storage_path_, current);
        const double cpu = have_previous && current.cpu.valid && previous.cpu.valid
            ? percent_delta(current.cpu.total - current.cpu.idle,
                            previous.cpu.total - previous.cpu.idle,
                            current.cpu.total, previous.cpu.total) : -1.0;
        // Keep proc_cpu on the same scale as cpu: percentage of total host
        // CPU capacity. The previous implementation multiplied by CLK_TCK
        // after normalizing by total ticks, inflating the result by ~100x.
        const double process_cpu = have_previous && current.cpu.valid && previous.cpu.valid
            ? 100.0 * static_cast<double>(current.cpu.process - previous.cpu.process)
              / static_cast<double>(std::max<uint64_t>(1, current.cpu.total - previous.cpu.total))
              : -1.0;

        const bool memory_high = current.ram_used_pct >= 85.0;
        const bool storage_high = current.disk_used_pct >= 80.0;
        double max_temp_c = current.cpu_temp_c;
        const char* max_temp_key = "cpu_temp";
        const auto consider_temp = [&](double value, const char* key) {
            if (value > max_temp_c) {
                max_temp_c = value;
                max_temp_key = key;
            }
        };
        consider_temp(current.gpu_temp_c, "gpu_temp");
        consider_temp(current.npu_temp_c, "npu_temp");
        consider_temp(current.ddr_temp_c, "ddr_temp");
        consider_temp(current.skin_temp_c, "skin_temp");
        char message[1024];
        int message_len = std::snprintf(
            message, sizeof(message),
            "uptime_s=%.0f cpu=%.1f%% proc_cpu=%.1f%% ram=%.1f%% rss_mb=%llu "
            "disk=%.1f%% %s=%.1fC load=%.2f/%.2f/%.2f "
            "threads=%d fd=%d",
            current.uptime_s, cpu, process_cpu, current.ram_used_pct,
            (unsigned long long)current.rss_mb, current.disk_used_pct,
            max_temp_key, max_temp_c,
            current.load1, current.load5, current.load15,
            current.threads, current.fd_count);
        if (message_len < 0) message_len = 0;
        if (message_len >= static_cast<int>(sizeof(message)))
            message_len = static_cast<int>(sizeof(message)) - 1;
        if (memory_high) {
            message_len += std::snprintf(
                message + message_len, sizeof(message) - message_len,
                " ram_avail_mb=%llu", (unsigned long long)current.ram_available_mb);
        }
        if (storage_high && message_len < static_cast<int>(sizeof(message))) {
            message_len += std::snprintf(
                message + message_len, sizeof(message) - message_len,
                " disk_free_gb=%.2f", current.disk_free_gb);
        }
        if (!complete && message_len < static_cast<int>(sizeof(message))) {
            std::snprintf(message + message_len, sizeof(message) - message_len,
                          " partial=true");
        }
        LOG_INFO("sys", "%s", message);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_warning >= std::chrono::minutes(1)) {
            if (process_cpu >= 90.0) {
                LOG_WARN("sys", "cpu_high proc_cpu=%.1f%%", process_cpu);
                last_warning = now;
            } else if (current.ram_used_pct >= 85.0) {
                LOG_WARN("sys", "memory_high ram=%.1f%% rss_mb=%llu",
                         current.ram_used_pct, (unsigned long long)current.rss_mb);
                last_warning = now;
            } else if (current.disk_used_pct >= 90.0) {
                LOG_WARN("sys", "storage_critical used=%.1f%% free_gb=%.2f",
                         current.disk_used_pct, current.disk_free_gb);
                last_warning = now;
            } else if (current.disk_used_pct >= 80.0) {
                LOG_WARN("sys", "storage_high used=%.1f%% free_gb=%.2f",
                         current.disk_used_pct, current.disk_free_gb);
                last_warning = now;
            } else if (max_temp_c >= 90.0) {
                LOG_WARN("sys", "temperature_critical %s=%.1fC", max_temp_key, max_temp_c);
                last_warning = now;
            } else if (max_temp_c >= 80.0) {
                LOG_WARN("sys", "temperature_high %s=%.1fC", max_temp_key, max_temp_c);
                last_warning = now;
            }
        }
        previous = current;
        have_previous = true;
    }
    LOG_INFO("sys", "monitor stopped");
}
