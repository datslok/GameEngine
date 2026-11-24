#include "scene/twig_set.h"

#include "math/median_split.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

std::size_t twigBricksFirstHalf(std::size_t bricks) {
    return medianSplitFirstHalf(bricks);
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
    // The twigs in bricks by their feet's median split: every brick but the last full.
    std::vector<Vec3> feet;
    feet.reserve(twigs.size());

    for (const TwigInstance& twig : twigs) {
        feet.push_back(twig.foot);
    }

    const std::vector<std::size_t> order = medianSplitOrder(feet, twigBrickSize);
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
