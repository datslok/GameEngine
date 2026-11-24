#include "math/median_split.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace {
    // A coordinate of a point by axis (0 x, 1 y, 2 z).
    float axisOf(const Vec3& point, int axis) {
        return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
    }

    // Splits order's first..end into groups, as medianSplitOrder says.
    void splitIntoGroups(std::vector<std::size_t>& order, std::size_t first, std::size_t end, const std::vector<Vec3>& points, std::size_t groupSize) {
        const std::size_t count = end - first;

        if (count <= groupSize) {
            return;
        }

        Vec3 low = points[order[first]];
        Vec3 high = low;

        for (std::size_t place = first; place < end; ++place) {
            const Vec3& point = points[order[place]];
            low = Vec3{std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
            high = Vec3{std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
        }

        const Vec3 size = high - low;
        const int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
        const std::size_t groups = (count + groupSize - 1) / groupSize;
        const std::size_t middle = first + medianSplitFirstHalf(groups) * groupSize;
        std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(first), order.begin() + static_cast<std::ptrdiff_t>(middle),
                         order.begin() + static_cast<std::ptrdiff_t>(end), [&](std::size_t a, std::size_t b) {
                             const float atA = axisOf(points[a], axis);
                             const float atB = axisOf(points[b], axis);
                             return atA < atB || (atA == atB && a < b);
                         });
        splitIntoGroups(order, first, middle, points, groupSize);
        splitIntoGroups(order, middle, end, points, groupSize);
    }
}

std::size_t medianSplitFirstHalf(std::size_t groups) {
    return (groups + 1) / 2;
}

std::vector<std::size_t> medianSplitOrder(const std::vector<Vec3>& points, std::size_t groupSize) {
    if (groupSize == 0) {
        throw std::invalid_argument("A median split's groups must hold at least one point");
    }

    std::vector<std::size_t> order(points.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    splitIntoGroups(order, 0, order.size(), points, groupSize);
    return order;
}
