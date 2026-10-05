#include <cstdio>
#include <vector>

#include "face_track_assoc.h"

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static Detection face(float x, float y, float w = 28, float h = 36) {
    Detection d{}; d.x1 = x; d.y1 = y; d.x2 = x + w; d.y2 = y + h; return d;
}
static TrackView track(int id, float x1, float y1, float x2, float y2) {
    TrackView t; t.id = id; t.state = ByteTrackState::Tracked; t.box = {x1, y1, x2, y2}; return t;
}

int main() {
    std::vector<TrackView> tracks{track(10, 0, 0, 100, 240), track(20, 90, 0, 190, 240)};
    std::vector<Detection> faces{face(34, 25), face(124, 25)};
    auto matched = associate_faces_to_tracks(faces, tracks);
    CHECK(matched == std::vector<int>({10, 20}), "two faces associate one-to-one to overlapping people");

    auto one_face = associate_faces_to_tracks({face(95, 25)}, tracks);
    CHECK(one_face.size() == 1 && one_face[0] >= 0, "one face maps to at most one track");

    auto orphan = associate_faces_to_tracks({face(20, 210)}, tracks);
    CHECK(orphan == std::vector<int>({-1}), "face outside head region remains orphan");

    tracks[0].predicted_only = true;
    auto predicted = associate_faces_to_tracks({face(34, 25)}, tracks);
    CHECK(predicted == std::vector<int>({-1}), "predicted-only track cannot receive new face association");

    std::vector<Detection> reordered{faces[1], faces[0]};
    auto reordered_match = associate_faces_to_tracks(reordered, {track(20, 90, 0, 190, 240), track(10, 0, 0, 100, 240)});
    CHECK(reordered_match == std::vector<int>({20, 10}), "geometry association is invariant to input order");

    std::printf("\n[test_face_track_assoc] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
