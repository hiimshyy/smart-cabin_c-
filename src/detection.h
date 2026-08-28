#pragma once
// Common face detection output shared by all detector backends.

#include <cstdint>

struct Detection {
    float x1, y1, x2, y2;    // bbox in original image coords
    float score;             // face confidence [0,1]
    float landmarks[10];     // (x0,y0,...,x4,y4) in original image coords
};
