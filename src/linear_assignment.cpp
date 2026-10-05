#include "linear_assignment.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

bool is_allowed(float value, float forbidden_cost) {
    return std::isfinite(value) && value < forbidden_cost;
}

}  // namespace

Assignment solve_linear_assignment(const std::vector<std::vector<float>>& cost,
                                   float forbidden_cost) {
    Assignment out;
    const int rows = static_cast<int>(cost.size());
    if (rows == 0) return out;

    const int cols = static_cast<int>(cost.front().size());
    for (const auto& row : cost) {
        if (static_cast<int>(row.size()) != cols)
            throw std::invalid_argument("linear assignment cost matrix is ragged");
    }
    if (cols == 0) {
        for (int row = 0; row < rows; ++row) out.unmatched_rows.push_back(row);
        return out;
    }

    // Give every row a private escape route.  This makes forbidden pairs truly
    // optional rather than letting a rectangular solver force one of them.
    float max_allowed = 0.0f;
    bool have_allowed = false;
    for (const auto& row : cost) {
        for (float value : row) {
            if (!is_allowed(value, forbidden_cost)) continue;
            max_allowed = have_allowed ? std::max(max_allowed, value) : value;
            have_allowed = true;
        }
    }
    const double dummy_cost = have_allowed
        ? static_cast<double>(max_allowed) + std::max(1.0, std::abs(static_cast<double>(max_allowed)) + 1.0)
        : 1.0;
    const double blocked_cost = dummy_cost + std::max(1.0, std::abs(dummy_cost) + 1.0);

    // Hungarian's potential formulation below requires columns >= rows.  The
    // added `rows` dummy columns satisfy that for every rectangular input.
    const int work_cols = cols + rows;
    std::vector<double> u(rows + 1, 0.0), v(work_cols + 1, 0.0);
    std::vector<int> p(work_cols + 1, 0), way(work_cols + 1, 0);

    auto work_cost = [&](int row_one_based, int col_one_based) -> double {
        const int row = row_one_based - 1;
        const int col = col_one_based - 1;
        if (col >= cols) return dummy_cost;
        const float value = cost[row][col];
        return is_allowed(value, forbidden_cost) ? static_cast<double>(value) : blocked_cost;
    };

    for (int row = 1; row <= rows; ++row) {
        p[0] = row;
        int col0 = 0;
        std::vector<double> minv(work_cols + 1, std::numeric_limits<double>::infinity());
        std::vector<bool> used(work_cols + 1, false);
        do {
            used[col0] = true;
            const int row0 = p[col0];
            double delta = std::numeric_limits<double>::infinity();
            int col1 = 0;
            for (int col = 1; col <= work_cols; ++col) {
                if (used[col]) continue;
                const double cur = work_cost(row0, col) - u[row0] - v[col];
                // Strict comparison preserves the lowest column index on ties.
                if (cur < minv[col]) {
                    minv[col] = cur;
                    way[col] = col0;
                }
                if (minv[col] < delta) {
                    delta = minv[col];
                    col1 = col;
                }
            }
            for (int col = 0; col <= work_cols; ++col) {
                if (used[col]) {
                    u[p[col]] += delta;
                    v[col] -= delta;
                } else {
                    minv[col] -= delta;
                }
            }
            col0 = col1;
        } while (p[col0] != 0);

        do {
            const int col1 = way[col0];
            p[col0] = p[col1];
            col0 = col1;
        } while (col0 != 0);
    }

    std::vector<int> row_to_col(rows, -1);
    for (int col = 1; col <= work_cols; ++col) {
        if (p[col] != 0) row_to_col[p[col] - 1] = col - 1;
    }
    std::vector<bool> column_matched(cols, false);
    for (int row = 0; row < rows; ++row) {
        const int col = row_to_col[row];
        if (col >= 0 && col < cols && is_allowed(cost[row][col], forbidden_cost)) {
            out.matches.emplace_back(row, col);
            column_matched[col] = true;
        } else {
            out.unmatched_rows.push_back(row);
        }
    }
    for (int col = 0; col < cols; ++col) {
        if (!column_matched[col]) out.unmatched_cols.push_back(col);
    }
    return out;
}
