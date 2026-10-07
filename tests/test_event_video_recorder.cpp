#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "event_video_recorder.h"

namespace fs = std::filesystem;

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; \
    std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output = fs::temp_directory_path() /
        ("face_cabin_recorder_" + std::to_string(suffix));
    fs::remove_all(output);

    EventVideoRecorderConfig config;
    config.output_dir = output.string();
    config.fps = 10.0;
    config.pre_roll_ms = 200.0;
    config.empty_stop_ms = 300.0;
    config.output_width = 32;
    config.cabin_id = 7;

    cv::Mat frame(48, 64, CV_8UC3, cv::Scalar(10, 20, 30));
    EventVideoRecorder recorder(config);
    recorder.push_frame(frame, 0.0);
    recorder.push_frame(frame, 100.0);
    recorder.push_frame(frame, 200.0);
    CHECK(!recorder.recording(), "empty cabin does not start a recording");

    recorder.set_person_present(true, 300.0);
    recorder.push_frame(frame, 300.0);
    CHECK(recorder.recording(), "confirmed person starts a recording");

    recorder.set_person_present(false, 300.0);
    recorder.push_frame(frame, 400.0);
    recorder.push_frame(frame, 500.0);
    CHECK(recorder.recording(), "short empty gap remains in the same clip");
    recorder.push_frame(frame, 600.0);
    CHECK(!recorder.recording(), "empty timeout closes the clip");
    recorder.close();  // wait until the asynchronous encoder has finalized the file
    CHECK(recorder.completed_segments() == 1, "one segment finalized");

    std::vector<fs::path> videos;
    bool has_partial = false;
    for (const auto& entry : fs::directory_iterator(output)) {
        if (entry.path().filename().string().find(".partial.avi") != std::string::npos)
            has_partial = true;
        else if (entry.path().extension() == ".avi")
            videos.push_back(entry.path());
    }
    CHECK(!has_partial, "successful close renames partial file");
    CHECK(videos.size() == 1, "one AVI file created");

    int decoded_frames = 0;
    int decoded_width = 0;
    if (!videos.empty()) {
        cv::VideoCapture capture(videos.front().string());
        cv::Mat decoded;
        while (capture.read(decoded)) {
            ++decoded_frames;
            decoded_width = decoded.cols;
        }
    }
    CHECK(decoded_frames >= 6, "clip includes pre-roll, person frame and empty tail");
    CHECK(decoded_width == 32, "encoder worker resizes to configured output width");

    fs::remove_all(output);
    std::printf("\n[test_event_video_recorder] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
