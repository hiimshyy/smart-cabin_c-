#pragma once

#include <utility>
#include <vector>

// Result of a one-to-one linear assignment.  Row/column indices always refer
// to the caller's original matrix, never an internally-added dummy column.
struct Assignment {
    std::vector<std::pair<int, int>> matches;
    std::vector<int> unmatched_rows;
    std::vector<int> unmatched_cols;
};

// Solve a rectangular minimum-cost assignment with a deterministic Hungarian
// implementation.  A matrix entry is forbidden when it is non-finite or
// greater than or equal to forbidden_cost.  Forbidden pairs are never emitted
// as matches; rows without a valid pair and unused columns are returned as
// unmatched.
Assignment solve_linear_assignment(const std::vector<std::vector<float>>& cost,
                                   float forbidden_cost);
