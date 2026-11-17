#include "traffic_db.h"

#include "log/logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::int64_t now_epoch_ms() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
        .count();
}

std::string format_utc_ts(std::int64_t epoch_ms) {
    if (epoch_ms <= 0) return "-";
    const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm           tm_buf{};
    gmtime_r(&secs, &tm_buf);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02dZ", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

bool parse_date_or_ms(const std::string& s, bool end_of_day, std::int64_t& out_ms) {
    if (s.empty()) return false;
    // Accept raw epoch milliseconds (all digits)
    bool all_digits = true;
    for (char c : s) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        out_ms = std::strtoll(s.c_str(), nullptr, 10);
        return true;
    }

    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1970 || m < 1 || m > 12 ||
        d < 1 || d > 31) {
        return false;
    }
    std::tm tm_buf{};
    tm_buf.tm_year         = y - 1900;
    tm_buf.tm_mon          = m - 1;
    tm_buf.tm_mday         = d;
    tm_buf.tm_hour         = end_of_day ? 23 : 0;
    tm_buf.tm_min          = end_of_day ? 59 : 0;
    tm_buf.tm_sec          = end_of_day ? 59 : 0;
    const std::time_t secs = timegm(&tm_buf);
    if (secs < 0) return false;
    out_ms = static_cast<std::int64_t>(secs) * 1000LL + (end_of_day ? 999LL : 0LL);
    return true;
}

bool confirm_action(const std::string& prompt, bool assume_yes) {
    if (assume_yes) return true;
    std::printf("%s [y/N]: ", prompt.c_str());
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

void print_global_help() {
    std::printf(
        "Usage: traffic_admin [--db PATH] [--schema PATH] <command> [options]\n"
        "\n"
        "Global options:\n"
        "  --db PATH       Path to traffic SQLite DB (default: db/traffic.db)\n"
        "  --schema PATH   Path to traffic schema SQL (default: db/traffic_schema.sql)\n"
        "  -h, --help      Show help (use '<command> --help' for subcommand examples)\n"
        "\n"
        "Commands:\n"
        "  visitors list     List tracked anonymous visitors\n"
        "  visitors show     Show visitor profile, embedding qualities, and recent trips\n"
        "  visitors merge    Merge two visitor profiles into one (audited)\n"
        "  visitors delete   Delete a visitor's biometrics and unlink face images (audited)\n"
        "  visitors promote  Promote a visitor into residents.db (audited)\n"
        "  visitors suggest  List high-similarity visitor <-> resident promotion candidates\n"
        "  export            Export traffic tables to CSV files (includes floor labels B, 1..9)\n"
        "\n"
        "Examples:\n"
        "  traffic_admin visitors list --status active --limit 20\n"
        "  traffic_admin visitors show 42\n"
        "  traffic_admin visitors suggest --resident-db db/residents.db\n"
        "  traffic_admin export --out-dir /tmp/traffic_csv --from 2026-11-01 --to 2026-11-03\n");
}

void print_visitors_help() {
    std::printf(
        "Usage: traffic_admin [--db PATH] visitors <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  list [--status active|all|expired|merged|promoted|deleted] [--limit N]\n"
        "  show <visitor_id>\n"
        "  merge <from_visitor_id> <into_visitor_id> [--yes]\n"
        "  delete <visitor_id> [--yes]\n"
        "  promote <visitor_id> (--resident-id <id> | --name <name> [--floor <n>] [--role "
        "<role>])\n"
        "          [--resident-db PATH] [--resident-schema PATH] [--yes]\n"
        "  suggest [--resident-db PATH] [--min-trips N] [--min-sim F] [--dim N]\n"
        "\n"
        "Examples:\n"
        "  traffic_admin visitors list --status active --limit 50\n"
        "  traffic_admin visitors show 12\n"
        "  traffic_admin visitors merge 15 12 --yes\n"
        "  traffic_admin visitors delete 18\n"
        "  traffic_admin visitors promote 12 --resident-id 7 --resident-db db/residents.db\n"
        "  traffic_admin visitors promote 14 --name \"Nguyen Van A\" --floor 7 --yes\n"
        "  traffic_admin visitors suggest --min-trips 3 --min-sim 0.35\n");
}

void print_export_help() {
    std::printf("Usage: traffic_admin [--db PATH] export --out-dir DIR [--from YYYY-MM-DD|MS] "
                "[--to YYYY-MM-DD|MS]\n"
                "\n"
                "Exports cabin_stops, cabin_runs, passenger_trips, car_calls, hall_calls, and "
                "visitors CSVs.\n"
                "Examples:\n"
                "  traffic_admin export --out-dir /mnt/usb/traffic_202611 --from 2026-11-01 --to "
                "2026-11-30\n");
}

int handle_visitors(TrafficDB& db, const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        print_visitors_help();
        return 0;
    }

    const std::string& sub = args[0];
    if (sub == "list") {
        std::string status = "active";
        int         limit  = 50;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-h" || args[i] == "--help") {
                print_visitors_help();
                return 0;
            } else if (args[i] == "--status" && i + 1 < args.size()) {
                status = args[++i];
            } else if (args[i] == "--limit" && i + 1 < args.size()) {
                limit = std::atoi(args[++i].c_str());
            } else {
                std::fprintf(stderr, "Unknown/incomplete option: %s\n", args[i].c_str());
                return 1;
            }
        }
        std::vector<VisitorSummary> rows;
        if (!db.list_visitors(status, limit, rows)) {
            std::fprintf(stderr, "Error querying visitors\n");
            return 1;
        }
        std::printf("%-6s %-10s %-6s %-5s %-6s %-20s %-20s\n", "ID", "STATUS", "TRIPS", "EMBS",
                    "MAX_Q", "FIRST_SEEN", "LAST_SEEN");
        for (const auto& v : rows) {
            std::printf("%-6lld %-10s %-6d %-5d %-6.2f %-20s %-20s\n", (long long)v.id,
                        v.status.c_str(), v.trip_count, v.embedding_count, v.max_quality,
                        format_utc_ts(v.first_seen_ms).c_str(),
                        format_utc_ts(v.last_seen_ms).c_str());
        }
        return 0;
    }

    if (sub == "show") {
        if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
            print_visitors_help();
            return args.size() >= 2 ? 0 : 1;
        }
        const std::int64_t vid = std::strtoll(args[1].c_str(), nullptr, 10);
        VisitorShowResult  res;
        if (!db.show_visitor(vid, res)) {
            std::fprintf(stderr, "Visitor %lld not found\n", (long long)vid);
            return 1;
        }
        std::printf("Visitor %lld: status=%s trips=%d embeddings=%d max_q=%.3f\n",
                    (long long)res.summary.id, res.summary.status.c_str(), res.summary.trip_count,
                    res.summary.embedding_count, res.summary.max_quality);
        std::printf("  first_seen=%s  last_seen=%s\n",
                    format_utc_ts(res.summary.first_seen_ms).c_str(),
                    format_utc_ts(res.summary.last_seen_ms).c_str());
        if (res.summary.merged_into > 0) {
            std::printf("  merged_into=%lld\n", (long long)res.summary.merged_into);
        }
        if (res.summary.promoted_resident_id > 0) {
            std::printf("  promoted_resident_id=%lld\n",
                        (long long)res.summary.promoted_resident_id);
        }
        std::printf("Embeddings (%zu):\n", res.embeddings.size());
        for (const auto& e : res.embeddings) {
            std::printf("  emb_id=%-6lld quality=%.3f created=%s\n", (long long)e.id, e.quality,
                        format_utc_ts(e.created_at_ms).c_str());
        }
        std::printf("Recent trips (%zu):\n", res.recent_trips.size());
        for (const auto& t : res.recent_trips) {
            std::printf("  trip=%s in=%s out=%s conf=%s sim=%.2f\n", t.trip_key.c_str(),
                        t.has_floor_in ? format_floor_label(t.floor_in).c_str() : "?",
                        t.has_floor_out ? format_floor_label(t.floor_out).c_str() : "?",
                        t.confidence.c_str(), t.identity_sim);
        }
        return 0;
    }

    if (sub == "merge") {
        std::vector<std::int64_t> ids;
        bool                      assume_yes = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-h" || args[i] == "--help") {
                print_visitors_help();
                return 0;
            } else if (args[i] == "--yes" || args[i] == "-y") {
                assume_yes = true;
            } else {
                ids.push_back(std::strtoll(args[i].c_str(), nullptr, 10));
            }
        }
        if (ids.size() != 2 || ids[0] <= 0 || ids[1] <= 0 || ids[0] == ids[1]) {
            std::fprintf(stderr,
                         "Usage: traffic_admin visitors merge <from_id> <into_id> [--yes]\n");
            return 1;
        }
        if (!confirm_action("Merge visitor " + std::to_string(ids[0]) + " into visitor " +
                                std::to_string(ids[1]) + "?",
                            assume_yes)) {
            std::printf("Cancelled.\n");
            return 0;
        }
        std::string err;
        if (!db.merge_visitors(ids[0], ids[1], now_epoch_ms(), 10, err)) {
            std::fprintf(stderr, "Merge failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("Merged visitor %lld into %lld.\n", (long long)ids[0], (long long)ids[1]);
        return 0;
    }

    if (sub == "delete") {
        std::int64_t vid        = -1;
        bool         assume_yes = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-h" || args[i] == "--help") {
                print_visitors_help();
                return 0;
            } else if (args[i] == "--yes" || args[i] == "-y") {
                assume_yes = true;
            } else {
                vid = std::strtoll(args[i].c_str(), nullptr, 10);
            }
        }
        if (vid <= 0) {
            std::fprintf(stderr, "Usage: traffic_admin visitors delete <visitor_id> [--yes]\n");
            return 1;
        }
        if (!confirm_action("Delete visitor " + std::to_string(vid) +
                                " biometric data and unlink face images?",
                            assume_yes)) {
            std::printf("Cancelled.\n");
            return 0;
        }
        std::vector<std::string> face_paths;
        std::string              err;
        if (!db.delete_visitor(vid, now_epoch_ms(), face_paths, err)) {
            std::fprintf(stderr, "Delete failed: %s\n", err.c_str());
            return 1;
        }
        int unlinked = 0;
        for (const auto& p : face_paths) {
            if (::unlink(p.c_str()) == 0) ++unlinked;
        }
        std::printf("Deleted visitor %lld (removed %d face image files).\n", (long long)vid,
                    unlinked);
        return 0;
    }

    if (sub == "promote") {
        PromoteRequest req;
        req.resident_db_path     = "db/residents.db";
        req.resident_schema_path = "db/schema.sql";
        bool assume_yes          = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-h" || args[i] == "--help") {
                print_visitors_help();
                return 0;
            } else if (args[i] == "--yes" || args[i] == "-y") {
                assume_yes = true;
            } else if (args[i] == "--resident-id" && i + 1 < args.size()) {
                req.existing_resident_id = std::strtoll(args[++i].c_str(), nullptr, 10);
            } else if (args[i] == "--name" && i + 1 < args.size()) {
                req.new_name = args[++i];
            } else if (args[i] == "--floor" && i + 1 < args.size()) {
                req.home_floor = std::atoi(args[++i].c_str());
            } else if (args[i] == "--role" && i + 1 < args.size()) {
                req.role = args[++i];
            } else if (args[i] == "--resident-db" && i + 1 < args.size()) {
                req.resident_db_path = args[++i];
            } else if (args[i] == "--resident-schema" && i + 1 < args.size()) {
                req.resident_schema_path = args[++i];
            } else if (req.visitor_id <= 0 && args[i][0] != '-') {
                req.visitor_id = std::strtoll(args[i].c_str(), nullptr, 10);
            } else {
                std::fprintf(stderr, "Unknown/incomplete option: %s\n", args[i].c_str());
                return 1;
            }
        }
        if (req.visitor_id <= 0 || (req.existing_resident_id <= 0 && req.new_name.empty())) {
            print_visitors_help();
            return 1;
        }
        const std::string target_desc =
            req.existing_resident_id > 0
                ? ("existing resident id=" + std::to_string(req.existing_resident_id))
                : ("new resident (floor=" + std::to_string(req.home_floor) + ")");
        if (!confirm_action("Promote visitor " + std::to_string(req.visitor_id) + " to " +
                                target_desc + " in " + req.resident_db_path + "?",
                            assume_yes)) {
            std::printf("Cancelled.\n");
            return 0;
        }
        req.now_ms              = now_epoch_ms();
        const PromoteResult res = db.promote_visitor(req);
        if (!res.ok) {
            std::fprintf(stderr, "Promote failed: %s\n", res.error.c_str());
            return 1;
        }
        std::printf("Promoted visitor %lld -> resident_id=%lld (copied %d embeddings, updated %lld "
                    "trips).\n",
                    (long long)req.visitor_id, (long long)res.resident_id, res.embeddings_copied,
                    (long long)res.trips_updated);
        return 0;
    }

    if (sub == "suggest") {
        std::string resident_db_path = "db/residents.db";
        int         min_trips        = 1;
        float       min_sim          = 0.30f;
        int         recog_dim        = 512;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-h" || args[i] == "--help") {
                print_visitors_help();
                return 0;
            } else if (args[i] == "--resident-db" && i + 1 < args.size()) {
                resident_db_path = args[++i];
            } else if (args[i] == "--min-trips" && i + 1 < args.size()) {
                min_trips = std::atoi(args[++i].c_str());
            } else if (args[i] == "--min-sim" && i + 1 < args.size()) {
                min_sim = static_cast<float>(std::atof(args[++i].c_str()));
            } else if (args[i] == "--dim" && i + 1 < args.size()) {
                recog_dim = std::atoi(args[++i].c_str());
            } else {
                std::fprintf(stderr, "Unknown/incomplete option: %s\n", args[i].c_str());
                return 1;
            }
        }
        std::vector<PromotionSuggestion> out;
        std::string                      err;
        if (!db.suggest_promotions(resident_db_path, recog_dim, min_trips, min_sim, out, err)) {
            std::fprintf(stderr, "Suggest failed: %s\n", err.c_str());
            return 1;
        }
        if (out.empty()) {
            std::printf("No promotion candidates found.\n");
            return 0;
        }
        std::printf("%-10s %-11s %-7s %-6s %-24s\n", "VISITOR", "RESIDENT", "MAX_SIM", "TRIPS",
                    "RESIDENT_NAME");
        for (const auto& s : out) {
            std::printf("%-10lld %-11lld %-7.3f %-6d %s (F%s)\n", (long long)s.visitor_id,
                        (long long)s.resident_id, s.max_similarity, s.trip_count,
                        s.resident_name.c_str(), format_floor_label(s.home_floor).c_str());
        }
        return 0;
    }

    std::fprintf(stderr, "Unknown visitors subcommand: %s\n", sub.c_str());
    print_visitors_help();
    return 1;
}

int handle_export(const TrafficDB& db, const std::vector<std::string>& args) {
    std::string  out_dir;
    std::int64_t from_ms = -1;
    std::int64_t to_ms   = -1;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-h" || args[i] == "--help") {
            print_export_help();
            return 0;
        } else if (args[i] == "--out-dir" && i + 1 < args.size()) {
            out_dir = args[++i];
        } else if (args[i] == "--from" && i + 1 < args.size()) {
            if (!parse_date_or_ms(args[++i], false, from_ms)) {
                std::fprintf(stderr, "Invalid --from date/timestamp\n");
                return 1;
            }
        } else if (args[i] == "--to" && i + 1 < args.size()) {
            if (!parse_date_or_ms(args[++i], true, to_ms)) {
                std::fprintf(stderr, "Invalid --to date/timestamp\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "Unknown/incomplete option: %s\n", args[i].c_str());
            return 1;
        }
    }
    if (out_dir.empty()) {
        std::fprintf(stderr, "--out-dir is required\n");
        print_export_help();
        return 1;
    }
    const TrafficExportResult res = db.export_csv(out_dir, from_ms, to_ms);
    if (!res.ok) {
        std::fprintf(stderr, "Export failed: %s\n", res.error.c_str());
        return 1;
    }
    std::printf("Exported CSVs to %s:\n", out_dir.c_str());
    for (const auto& kv : res.row_counts) {
        std::printf("  %s.csv: %d rows\n", kv.first.c_str(), kv.second);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    LogConfig lcfg;
    lcfg.level     = LogLevel::WARN;
    lcfg.to_stderr = true;
    lcfg.to_file   = false;
    Logger::instance().init(lcfg);

    std::string              db_path     = "db/traffic.db";
    std::string              schema_path = "db/traffic_schema.sql";
    std::vector<std::string> rest;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (a == "--schema" && i + 1 < argc) {
            schema_path = argv[++i];
        } else {
            for (int j = i; j < argc; ++j) {
                rest.emplace_back(argv[j]);
            }
            break;
        }
    }

    if (rest.empty() || rest[0] == "-h" || rest[0] == "--help") {
        print_global_help();
        return 0;
    }

    TrafficDB db;
    if (!db.open(db_path, schema_path)) {
        std::fprintf(stderr, "Cannot open traffic DB: %s\n", db_path.c_str());
        return 1;
    }

    const std::string        cmd = rest[0];
    std::vector<std::string> sub_args(rest.begin() + 1, rest.end());
    int                      rc = 0;
    if (cmd == "visitors") {
        rc = handle_visitors(db, sub_args);
    } else if (cmd == "export") {
        rc = handle_export(db, sub_args);
    } else {
        std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
        print_global_help();
        rc = 1;
    }
    db.close();
    return rc;
}
