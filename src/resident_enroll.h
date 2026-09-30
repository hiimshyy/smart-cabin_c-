#pragma once

#include <cstdint>
#include <string>
#include <vector>

class ResidentDB;

// Pure SQLite enrollment batch (no NPU/OpenCV). Extracting face embeddings
// happens before this API is called; this function makes the database mutation
// all-or-nothing so a hot-reload reader never observes zero/partial vectors.
struct ResidentEnrollRequest {
    std::string ext_id;       // non-empty => key/upsert by ext_id; empty => legacy name key
    std::string name;
    int         home_floor = 0;
    std::string role = "staff";
    std::string source = "id_photo";
    bool        replace = false;
    std::vector<std::vector<float>> embeddings;
};

struct ResidentEnrollResult {
    bool        ok = false;
    int64_t     resident_id = -1;
    int         embeddings_written = 0;
    std::string error;        // stable machine-readable code; never contains PII
};

ResidentEnrollResult write_resident_enrollment(ResidentDB& db,
                                                const ResidentEnrollRequest& req);
