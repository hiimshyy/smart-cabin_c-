#include "face_track_assoc.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "linear_assignment.h"

namespace {

constexpr float kForbiddenCost = 1e6f;

bool less_face(const Detection& a, const Detection& b) {
    if (a.x1 != b.x1) return a.x1 < b.x1;
    if (a.y1 != b.y1) return a.y1 < b.y1;
    if (a.x2 != b.x2) return a.x2 < b.x2;
    return a.y2 < b.y2;
}

bool candidate_cost(const Detection& face, const TrackView& track,
                    const FaceTrackAssocConfig& cfg, float& cost) {
    if (track.state != ByteTrackState::Tracked || track.predicted_only) return false;
    const BoxF& person = track.box;
    const float person_w = person.x2 - person.x1;
    const float person_h = person.y2 - person.y1;
    const float face_w = face.x2 - face.x1;
    const float face_h = face.y2 - face.y1;
    if (!(person_w > 0.0f && person_h > 0.0f && face_w > 0.0f && face_h > 0.0f)) return false;
    const float fx = (face.x1 + face.x2) * 0.5f;
    const float fy = (face.y1 + face.y2) * 0.5f;
    const float margin_x = cfg.head_margin_ratio * person_w;
    const float margin_y = cfg.head_margin_ratio * person_h;
    const float head_h = cfg.head_ratio * person_h;
    if (fx < person.x1 - margin_x || fx > person.x2 + margin_x ||
        fy < person.y1 - margin_y || fy > person.y1 + head_h + margin_y)
        return false;
    const float width_ratio = face_w / person_w;
    const float height_ratio = face_h / person_h;
    if (width_ratio < cfg.min_face_width_ratio || width_ratio > cfg.max_face_width_ratio ||
        height_ratio < cfg.min_face_height_ratio || height_ratio > cfg.max_face_height_ratio)
        return false;

    const float expected_x = (person.x1 + person.x2) * 0.5f;
    const float expected_y = person.y1 + head_h * 0.5f;
    const float dx = std::abs(fx - expected_x) / person_w;
    const float dy = std::abs(fy - expected_y) / std::max(1.0f, head_h);
    const float size_penalty = std::abs(width_ratio - 0.25f) + std::abs(height_ratio - 0.18f);
    cost = dx + dy + 0.25f * size_penalty;
    return true;
}

}  // namespace

std::vector<int> associate_faces_to_tracks(const std::vector<Detection>& faces,
                                           const std::vector<TrackView>& tracks,
                                           const FaceTrackAssocConfig& config) {
    std::vector<int> result(faces.size(), -1);
    if (faces.empty() || tracks.empty()) return result;

    std::vector<int> face_order(faces.size());
    std::iota(face_order.begin(), face_order.end(), 0);
    std::sort(face_order.begin(), face_order.end(), [&](int a, int b) {
        return less_face(faces[a], faces[b]);
    });
    std::vector<int> track_order(tracks.size());
    std::iota(track_order.begin(), track_order.end(), 0);
    std::sort(track_order.begin(), track_order.end(), [&](int a, int b) {
        return tracks[a].id < tracks[b].id;
    });

    std::vector<std::vector<float>> cost(face_order.size(),
                                         std::vector<float>(track_order.size(), kForbiddenCost));
    for (size_t row = 0; row < face_order.size(); ++row) {
        for (size_t col = 0; col < track_order.size(); ++col) {
            float value = 0.0f;
            if (candidate_cost(faces[face_order[row]], tracks[track_order[col]], config, value))
                cost[row][col] = value;
        }
    }
    Assignment assignment = solve_linear_assignment(cost, kForbiddenCost);
    for (const auto& pair : assignment.matches)
        result[face_order[pair.first]] = tracks[track_order[pair.second]].id;
    return result;
}
