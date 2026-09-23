#pragma once
#include <cstdint>
#include <string>

// Per-face pipeline label (spec main-cpp-refactor). This is the single
// documented intentional structural addition (Req 6.4): the struct was a local
// definition inside main.cpp's pipeline loop; it is promoted to a shared
// data-only header so both main.cpp (which produces it) and overlay.cpp (which
// reads it) can name the type. No logic — fields and defaults are byte-for-byte
// identical to the Baseline local struct.
struct FaceLabel {
    std::string name;
    float       sim = -1.0f;
    char        stat = '.';
    int         track_id = -1;
    int64_t     resident_id = -1;   // resident-db mode
    float       area = 0.0f;        // face bbox area (largest-face subject)
};
