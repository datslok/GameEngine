#include "math/percentile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

float nearestRank(std::vector<float> values, float share) {
    if (!(share > 0.0f && share <= 1.0f)) {
        throw std::invalid_argument("A nearest rank's share must be in (0, 1]");
    }

    if (values.empty()) {
        return 0.0f;
    }

    // A float share is a little off (0.6f is 0.60000002), so a place within a ten-thousandth over a whole one is that
    // one: 0.6 of 5 values is place 3, not 4.
    const double place = std::ceil(static_cast<double>(share) * static_cast<double>(values.size()) - 1e-4);
    const auto at = values.begin() + std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(place), 1, static_cast<std::ptrdiff_t>(values.size())) - 1;
    std::nth_element(values.begin(), at, values.end());
    return *at;
}
