#include "scene/twig_set.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace {
    // A coordinate of a point by axis (0 x, 1 y, 2 z).
    float axisOf(const Vec3& point, int axis) {
        return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
    }

    /*
    * The twigs numbered first..end of order split into bricks: while more than a brick's worth, split along the longest
    * side of their feet's box at a whole number of bricks (half the bricks, rounded up), the lower feet first (ties by
    * their number, so the order never depends on the sort's), each half again. Every brick but the last is full.
    */
    void splitIntoBricks(std::vector<std::size_t>& order, std::size_t first, std::size_t end, const std::vector<TwigInstance>& twigs) {
        const std::size_t count = end - first;

        if (count <= twigBrickSize) {
            return;
        }

        Vec3 low = twigs[order[first]].foot;
        Vec3 high = low;

        for (std::size_t place = first; place < end; ++place) {
            const Vec3& foot = twigs[order[place]].foot;
            low = Vec3{std::min(low.x, foot.x), std::min(low.y, foot.y), std::min(low.z, foot.z)};
            high = Vec3{std::max(high.x, foot.x), std::max(high.y, foot.y), std::max(high.z, foot.z)};
        }

        const Vec3 size = high - low;
        const int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
        const std::size_t bricks = (count + twigBrickSize - 1) / twigBrickSize;
        const std::size_t middle = first + (bricks + 1) / 2 * twigBrickSize;
        std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(first), order.begin() + static_cast<std::ptrdiff_t>(middle),
                         order.begin() + static_cast<std::ptrdiff_t>(end), [&](std::size_t a, std::size_t b) {
                             const float atA = axisOf(twigs[a].foot, axis);
                             const float atB = axisOf(twigs[b].foot, axis);
                             return atA < atB || (atA == atB && a < b);
                         });
        splitIntoBricks(order, first, middle, twigs);
        splitIntoBricks(order, middle, end, twigs);
    }
}

float meshReach(const IndexedMesh& mesh) {
    float reach = 0.0f;

    for (const MeshVertex& vertex : mesh.vertices) {
        reach = std::max(reach, vertex.position.length());
    }

    return reach;
}

Vec3 twigPoint(const TwigInstance& twig, const Vec3& local) {
    return twig.foot + (twig.across * local.x + twig.towardsTip * local.y + twig.facing * local.z) * twig.scale;
}

/*
* A brick's sphere: centred on its twigs' feet's box, its radius the farthest twig's foot plus that twig's reach, so it
* holds every twig's sphere exactly (no fit to trust).
*/
TwigSet makeTwigSet(std::vector<MeshHandle> prototypes, const std::vector<float>& reaches, std::vector<TwigInstance> twigs) {
    for (const TwigInstance& twig : twigs) {
        if (twig.prototype >= reaches.size()) {
            throw std::invalid_argument("A twig's prototype has no reach");
        }
    }

    TwigSet set;
    set.prototypes = std::move(prototypes);
    std::vector<std::size_t> order(twigs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    splitIntoBricks(order, 0, order.size(), twigs);
    set.twigs.reserve(twigs.size());

    for (const std::size_t index : order) {
        set.twigs.push_back(twigs[index]);
    }

    for (std::size_t first = 0; first < set.twigs.size(); first += twigBrickSize) {
        const std::size_t end = std::min(first + twigBrickSize, set.twigs.size());
        Vec3 boxLow = set.twigs[first].foot;
        Vec3 boxHigh = boxLow;

        for (std::size_t twig = first; twig < end; ++twig) {
            const Vec3& foot = set.twigs[twig].foot;
            boxLow = Vec3{std::min(boxLow.x, foot.x), std::min(boxLow.y, foot.y), std::min(boxLow.z, foot.z)};
            boxHigh = Vec3{std::max(boxHigh.x, foot.x), std::max(boxHigh.y, foot.y), std::max(boxHigh.z, foot.z)};
        }

        TwigBrick brick;
        brick.centre = (boxLow + boxHigh) * 0.5f;
        brick.firstTwig = static_cast<std::uint32_t>(first);
        brick.twigCount = static_cast<std::uint32_t>(end - first);

        for (std::size_t twig = first; twig < end; ++twig) {
            const TwigInstance& placed = set.twigs[twig];
            brick.radius = std::max(brick.radius, (placed.foot - brick.centre).length() + reaches[placed.prototype] * placed.scale);
        }

        set.bricks.push_back(brick);
    }

    return set;
}
