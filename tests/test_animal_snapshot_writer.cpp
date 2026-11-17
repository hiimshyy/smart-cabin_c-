#include "animal_snapshot_writer.h"

#include <opencv2/core.hpp>

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int checks = 0;
int failed = 0;

#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failed;                                                                              \
            std::printf("  FAIL: %s (line %d)\n", m, __LINE__);                                    \
        }                                                                                          \
    } while (0)

void find_jpgs(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir;
        full += '/';
        full += name;
        struct stat st {};
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            find_jpgs(full, out);
        } else if (S_ISREG(st.st_mode) && full.size() > 4 &&
                   full.substr(full.size() - 4) == ".jpg") {
            out.push_back(full);
        }
    }
    ::closedir(d);
}

} // namespace

int main() {
    const std::string base_dir = "/tmp/scabin_tests/animal_snap_" + std::to_string(::getpid());

    AnimalSnapshotWriterConfig cfg;
    cfg.output_dir = base_dir;
    cfg.max_queue  = 4;
    AnimalSnapshotWriter writer(cfg);
    writer.start();

    cv::Mat                img(120, 160, CV_8UC3, cv::Scalar(40, 80, 120));
    std::vector<AnimalDet> dets;
    dets.push_back(AnimalDet{20.0f, 30.0f, 100.0f, 110.0f, 0.88f, YoloDecoder::kCocoDog});
    writer.enqueue(img, dets, 2, "dog", 0.88f);
    writer.close();

    CHECK(writer.saved_count() == 1 && writer.dropped_count() == 0,
          "close flushes enqueued animal snapshot");

    std::vector<std::string> jpgs;
    find_jpgs(base_dir, jpgs);
    CHECK(jpgs.size() == 1, "one JPEG snapshot file created on disk");
    if (!jpgs.empty()) {
        struct stat st {};
        CHECK(::stat(jpgs[0].c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size > 0,
              "saved animal JPEG has mode 0600 and non-zero size");
    }

    const std::string rm_cmd = "rm -rf '" + base_dir + "'";
    std::system(rm_cmd.c_str());

    std::printf("\n[test_animal_snapshot_writer] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
