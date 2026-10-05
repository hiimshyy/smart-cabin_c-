#include "resident_reconcile.h"

#include <set>
#include <utility>

CatalogReconcileResult reconcile_catalog_runtime(
    const CatalogDiff& diff, PersonTracker* tracker, InteractionManager& interaction) {
    CatalogReconcileResult out;
    std::set<int64_t> changed_or_removed = diff.changed;
    changed_or_removed.insert(diff.removed.begin(), diff.removed.end());

    if (tracker) {
        const bool invalidate_unknown = !diff.added.empty();
        out.affected_subject_keys = tracker->invalidate_residents(changed_or_removed, invalidate_unknown);
        out.invalidated_live_tracks = static_cast<int>(out.affected_subject_keys.size());
        interaction.drop_sessions(out.affected_subject_keys);
        return out;
    }

    // SCRFD-only mode has no persistent tracker identity.  Only a current
    // session whose resident changed/was removed needs reconciliation.
    for (const auto& session : interaction.sessions()) {
        if (session.subject_key == 0 && changed_or_removed.count(session.resident_id)) {
            out.affected_subject_keys.push_back(0);
            break;
        }
    }
    if (!out.affected_subject_keys.empty()) interaction.drop_sessions(out.affected_subject_keys);
    return out;
}

CatalogReconcileResult reconcile_catalog_runtime(
    const CatalogDiff& diff,
    bool use_tracker,
    Tracker& tracker,
    std::map<int, int64_t>& track_resident_id,
    InteractionManager& interaction) {
    CatalogReconcileResult out;

    std::set<int64_t> changed_or_removed = diff.changed;
    changed_or_removed.insert(diff.removed.begin(), diff.removed.end());

    if (use_tracker) {
        std::set<int> affected_track_ids;
        for (const auto& [track_id, resident_id] : track_resident_id) {
            if (changed_or_removed.count(resident_id))
                affected_track_ids.insert(track_id);
        }

        // Added residents may match subjects previously cached as unknown.
        const bool invalidate_unknown = !diff.added.empty();
        std::vector<int> invalidated_live = tracker.invalidate_catalog_cache(
            affected_track_ids, invalidate_unknown); // also purges all ghosts
        out.invalidated_live_tracks = static_cast<int>(invalidated_live.size());

        // Reconcile the union: affected mappings may outlive their Track, while
        // newly invalidated unknown live tracks may have no mapping at all.
        std::set<int> all_subject_keys = affected_track_ids;
        all_subject_keys.insert(invalidated_live.begin(), invalidated_live.end());
        out.affected_subject_keys.assign(all_subject_keys.begin(), all_subject_keys.end());
        for (int track_id : out.affected_subject_keys) {
            out.erased_track_mappings += static_cast<int>(track_resident_id.erase(track_id));
        }
        interaction.drop_sessions(out.affected_subject_keys);
        return out;
    }

    // SCRFD-only mode runs recognition every frame. An unrelated addition does
    // not require dropping key 0 and must not duplicate a confirmed action.
    // Reconcile only if key 0 currently references changed/removed content.
    std::vector<int> affected;
    for (const auto& s : interaction.sessions()) {
        if (s.subject_key == 0 && changed_or_removed.count(s.resident_id)) {
            affected.push_back(0);
            break;
        }
    }
    if (!affected.empty()) interaction.drop_sessions(affected);
    out.affected_subject_keys = std::move(affected);
    return out;
}
