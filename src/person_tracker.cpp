#include "person_tracker.h"

#include "byte_tracker.h"
#include "tracker_greedy.h"

namespace {

class BytePersonTracker final : public PersonTracker {
public:
    explicit BytePersonTracker(TrackerConfig config) : impl_(config) {}
    void predict(const FrameContext& frame) override { impl_.predict(frame); }
    void update(const std::vector<PersonDet>& detections, const FrameContext& frame) override {
        impl_.update(detections, frame);
    }
    std::vector<TrackView> active_tracks() const override { return impl_.active_tracks(); }
    std::optional<TrackView> find_track(int id) const override { return impl_.find_track(id); }
    bool needs_recognition(int id, double now_ms) const override {
        return impl_.needs_recognition(id, now_ms);
    }
    void record_recognition(int id, int64_t resident_id, const std::string& label,
                            float similarity, double now_ms) override {
        impl_.record_recognition(id, resident_id, label, similarity, now_ms);
    }
    std::vector<int> invalidate_residents(const std::set<int64_t>& ids,
                                          bool invalidate_unknown) override {
        return impl_.invalidate_residents(ids, invalidate_unknown);
    }
    int active_count() const override { return impl_.active_count(); }
    int tracked_count() const override { return impl_.tracked_count(); }
private:
    ByteTracker impl_;
};

}  // namespace

std::unique_ptr<PersonTracker> make_person_tracker(TrackerMode mode,
                                                   const TrackerConfig& config,
                                                   int legacy_max_missed,
                                                   int legacy_recognition_retry_frames) {
    if (mode == TrackerMode::Greedy)
        return std::make_unique<GreedyIouTracker>(config, legacy_max_missed,
                                                  legacy_recognition_retry_frames);
    return std::make_unique<BytePersonTracker>(config);
}
