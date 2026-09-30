#include "resident_enroll.h"

#include "resident_db.h"

ResidentEnrollResult write_resident_enrollment(ResidentDB& db,
                                                const ResidentEnrollRequest& req) {
    ResidentEnrollResult out;
    if (!db.is_open()) {
        out.error = "db_not_open";
        return out;
    }
    if (req.embeddings.empty()) {
        out.error = "no_embeddings";
        return out;
    }
    if (!db.begin()) {
        out.error = "db_begin_failed";
        return out;
    }

    // Every return after begin() must roll back. Keep this helper explicit and
    // small so the transaction boundary is easy to audit.
    auto fail = [&](const char* error) {
        db.rollback();
        out.error = error;
        out.resident_id = -1;
        out.embeddings_written = 0;
        return out;
    };

    const std::string role = req.role.empty() ? std::string("staff") : req.role;
    int64_t resident_id = req.ext_id.empty()
        ? db.upsert_resident(req.name, req.home_floor)
        : db.upsert_resident_by_ext_id(req.ext_id, req.name, req.home_floor, role);
    if (resident_id < 0) return fail("db_upsert_failed");

    if (req.replace && !db.delete_embeddings(resident_id)) {
        return fail("db_delete_failed");
    }

    int written = 0;
    for (const auto& embedding : req.embeddings) {
        if (!db.add_embedding(resident_id, req.source, embedding)) {
            return fail("db_write_failed");
        }
        ++written;
    }
    if (written != static_cast<int>(req.embeddings.size())) {
        return fail("db_write_failed");
    }

    if (!db.commit()) {
        // COMMIT failure may leave the transaction active; rollback is safe to
        // attempt and preserves the contract that ok=true means durable commit.
        db.rollback();
        out.error = "db_commit_failed";
        return out;
    }

    out.ok = true;
    out.resident_id = resident_id;
    out.embeddings_written = written;
    return out;
}
