#include <cstdio>
#include <limits>
#include <vector>

#include "linear_assignment.h"

static int checks = 0;
static int failed = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static bool has(const Assignment& a, int row, int col) {
    for (const auto& pair : a.matches)
        if (pair.first == row && pair.second == col) return true;
    return false;
}

int main() {
    constexpr float kForbidden = 1e6f;
    Assignment square = solve_linear_assignment({{4, 1}, {2, 3}}, kForbidden);
    CHECK(square.matches.size() == 2, "square matrix has two matches");
    CHECK(has(square, 0, 1) && has(square, 1, 0), "square optimum selected");

    Assignment wide = solve_linear_assignment({{5, 1, 9, 8}, {2, 7, 3, 4}}, kForbidden);
    CHECK(wide.matches.size() == 2, "wide matrix matches each row");
    CHECK(has(wide, 0, 1) && has(wide, 1, 0), "wide optimum selected");
    CHECK(wide.unmatched_cols.size() == 2, "wide matrix reports unused columns");

    Assignment tall = solve_linear_assignment({{1, 9}, {2, 8}, {9, 1}, {8, 2}}, kForbidden);
    CHECK(tall.matches.size() == 2, "tall matrix matches only available columns");
    CHECK(has(tall, 0, 0) && has(tall, 2, 1), "tall optimum selected");
    CHECK(tall.unmatched_rows.size() == 2, "tall matrix reports unused rows");

    Assignment forbidden = solve_linear_assignment({{0.2f, kForbidden},
                                                     {std::numeric_limits<float>::quiet_NaN(), kForbidden}},
                                                    kForbidden);
    CHECK(forbidden.matches.size() == 1 && has(forbidden, 0, 0), "forbidden pair is never matched");
    CHECK(forbidden.unmatched_rows == std::vector<int>{1}, "forbidden row becomes unmatched");
    CHECK(forbidden.unmatched_cols == std::vector<int>{1}, "forbidden column remains unmatched");

    Assignment equal = solve_linear_assignment({{1, 1}, {1, 1}}, kForbidden);
    CHECK(has(equal, 0, 0) && has(equal, 1, 1), "equal costs use deterministic row/column tie break");

    Assignment empty_rows = solve_linear_assignment({}, kForbidden);
    CHECK(empty_rows.matches.empty() && empty_rows.unmatched_rows.empty(), "empty rows accepted");
    Assignment empty_cols = solve_linear_assignment({{}}, kForbidden);
    CHECK(empty_cols.unmatched_rows == std::vector<int>{0}, "empty columns report unmatched row");

    std::printf("\n[test_linear_assignment] %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
