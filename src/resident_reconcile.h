#pragma once

#include <map>
#include <vector>

#include "interaction.h"
#include "resident_catalog.h"
#include "tracker.h"

struct CatalogReconcileResult {
    std::vector<int> affected_subject_keys;
    int invalidated_live_tracks = 0;
    int erased_track_mappings = 0;
};

// Reconcile all recognition-derived runtime state at the same frame-boundary
// safe point as a resident catalog swap. This central helper prevents callers
// from clearing Tracker while forgetting the authoritative track->resident map
// or InteractionManager sessions.
CatalogReconcileResult reconcile_catalog_runtime(
    const CatalogDiff& diff,
    bool use_tracker,
    Tracker& tracker,
    std::map<int, int64_t>& track_resident_id,
    InteractionManager& interaction);
