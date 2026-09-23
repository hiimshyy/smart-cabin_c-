#pragma once
#include <algorithm>

// Per-stage timing accumulator (moved verbatim from main.cpp — spec
// main-cpp-refactor, Benchmark_Module).
struct StageStat {
    double sum = 0.0, mn = 1e9, mx = 0.0;
    int    n   = 0;
    void   add(double v) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); ++n; }
    double avg() const { return n ? sum / n : 0.0; }
};

// Prints the "===== BENCHMARK SUMMARY =====" block to stdout using the exact
// Baseline printf format specifiers, row labels, and column ordering, including
// the conditional `yolo (Nx)` row gated on `use_tracker && yolo.n > 0`.
void print_benchmark_summary(double run_secs, int frame_id,
                             bool use_tracker, int person_every,
                             const StageStat& cap, const StageStat& pre,
                             const StageStat& yolo, const StageStat& scrfd,
                             const StageStat& recog, const StageStat& draw,
                             const StageStat& e2e);
