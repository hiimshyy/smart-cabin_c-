#include "benchmark.h"
#include <cstdio>

// Moved verbatim from main.cpp (spec main-cpp-refactor). Every printf line —
// row labels, column ordering, and numeric format specifiers — is byte-for-byte
// identical to the Baseline, including the conditional `yolo (%dx)` row.
void print_benchmark_summary(double run_secs, int frame_id,
                             bool use_tracker, int person_every,
                             const StageStat& s_cap, const StageStat& s_pre,
                             const StageStat& s_yolo, const StageStat& s_scrfd,
                             const StageStat& s_recog, const StageStat& s_draw,
                             const StageStat& s_e2e) {
    printf("\n===== BENCHMARK SUMMARY =====\n");
    printf("Duration    : %.2f s\n", run_secs);
    printf("Frames done : %d\n", frame_id);
    printf("Avg FPS     : %.2f\n", frame_id / (run_secs > 0 ? run_secs : 1));
    printf("Stage        avg (ms)   min       max\n");
    printf("capture    %8.2f  %8.2f  %8.2f\n", s_cap.avg(),   s_cap.mn,   s_cap.mx);
    printf("preprocess %8.2f  %8.2f  %8.2f\n", s_pre.avg(),   s_pre.mn,   s_pre.mx);
    if (use_tracker && s_yolo.n > 0) {
        printf("yolo (%dx)  %8.2f  %8.2f  %8.2f\n",
               person_every, s_yolo.avg(), s_yolo.mn, s_yolo.mx);
    }
    printf("scrfd      %8.2f  %8.2f  %8.2f\n", s_scrfd.avg(), s_scrfd.mn, s_scrfd.mx);
    printf("recog+match%8.2f  %8.2f  %8.2f\n", s_recog.avg(), s_recog.mn, s_recog.mx);
    printf("draw+show  %8.2f  %8.2f  %8.2f\n", s_draw.avg(),  s_draw.mn,  s_draw.mx);
    printf("end-to-end %8.2f  %8.2f  %8.2f\n", s_e2e.avg(),   s_e2e.mn,   s_e2e.mx);
    printf("=============================\n");
}
