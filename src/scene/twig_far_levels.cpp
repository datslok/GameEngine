#include "scene/twig_far_levels.h"

#include "math/percentile.h"
#include "scene/leaf_pruning.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
    // The set's handed-over leaves in brick and twig order (the records of the bricks' finest levels), each with what
    // its levels need: its area at the finest growth, its finest side and its reach (plant's frame), and its rank.
    struct RankedLeaves {
        std::vector<FarLeaf> leaves;
        std::vector<float> area;
        std::vector<float> finestSide;
        std::vector<float> reach;
        std::vector<std::uint32_t> rank;
        std::vector<std::uint32_t> brickFirstLeaf; // a brick's first leaf, and the count after the last
    };

    RankedLeaves rankLeaves(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes) {
        RankedLeaves ranked;

        for (const TwigBrick& brick : set.bricks) {
            ranked.brickFirstLeaf.push_back(static_cast<std::uint32_t>(ranked.leaves.size()));

            for (std::uint32_t number = brick.firstTwig; number < brick.firstTwig + brick.twigCount; ++number) {
                const TwigInstance& twig = set.twigs[number];
                const TwigFarLeaves& handed = prototypes[twig.prototype];

                for (const TwigLeafPlace& leaf : handed.leaves) {
                    const FarLeaf record = farLeafOf(twig, number, leaf, 1.0f);
                    ranked.leaves.push_back(record);
                    ranked.area.push_back(leaf.area * record.side * record.side);
                    ranked.finestSide.push_back(record.side / handed.growth);
                    ranked.reach.push_back(leaf.reach);
                }
            }
        }

        ranked.brickFirstLeaf.push_back(static_cast<std::uint32_t>(ranked.leaves.size()));
        const auto count = static_cast<std::uint32_t>(ranked.leaves.size());
        const std::vector<std::uint32_t> order = prunedOrder(count, 0);
        ranked.rank.resize(count);

        for (std::uint32_t place = 0; place < count; ++place) {
            ranked.rank[order[place]] = place;
        }

        return ranked;
    }

    /*
    * Builds the far tree, node by node (children before parents, so a parent's first density and error floor are
    * known). Each node's levels are made from its leaves' run, the bricks' finest leaves under it.
    */
    class FarTreeBuilder {
    public:
        FarTreeBuilder(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, const RankedLeaves& ranked, float errorShare, TwigFarLevels& far)
            : set(set), prototypes(prototypes), ranked(ranked), errorShare(errorShare), far(far) {
            far.nodes.resize(set.bricks.size());
        }

        // The node over bricks first..first + count: a brick's own, or a merge of the split's two halves.
        std::uint32_t build(std::uint32_t firstBrick, std::uint32_t brickCount) {
            if (brickCount == 1) {
                TwigFarNode& node = far.nodes[firstBrick];
                node.firstBrick = firstBrick;
                node.brickCount = 1;
                makeLevels(firstBrick, 0, brickErrorFloor(firstBrick));
                return firstBrick;
            }

            const auto firstHalf = static_cast<std::uint32_t>(twigBricksFirstHalf(brickCount));
            const std::uint32_t first = build(firstBrick, firstHalf);
            const std::uint32_t second = build(firstBrick + firstHalf, brickCount - firstHalf);
            const auto index = static_cast<std::uint32_t>(far.nodes.size());
            TwigFarNode& node = far.nodes.emplace_back();
            node.firstBrick = firstBrick;
            node.brickCount = brickCount;
            node.children[0] = first;
            node.children[1] = second;
            std::uint32_t density = 0;
            float floor = 0.0f;

            for (const std::uint32_t child : {first, second}) {
                far.nodes[child].parent = index;
                const TwigFarLevel& last = lastLevel(far.nodes[child]);
                density = std::max(density, last.density + 1);
                floor = std::max(floor, last.error);
            }

            makeLevels(index, density, floor);
            return index;
        }

    private:
        const TwigSet& set;
        const std::vector<TwigFarLeaves>& prototypes;
        const RankedLeaves& ranked;
        float errorShare; // the error percentile as a share
        TwigFarLevels& far;

        const TwigFarLevel& lastLevel(const TwigFarNode& node) const {
            return far.levels[node.firstLevel + node.levelCount - 1];
        }

        // A brick's finest level is its twigs': their level's error (prototype's frame) times their scale, once for each
        // leaf they hand over, at the error percentile.
        float brickErrorFloor(std::uint32_t brick) const {
            std::vector<float> errors;
            const TwigBrick& twigs = set.bricks[brick];

            for (std::uint32_t number = twigs.firstTwig; number < twigs.firstTwig + twigs.twigCount; ++number) {
                const TwigInstance& twig = set.twigs[number];
                const TwigFarLeaves& handed = prototypes[twig.prototype];
                errors.insert(errors.end(), handed.leaves.size(), handed.error * twig.scale);
            }

            return nearestRank(std::move(errors), errorShare);
        }

        /*
        * A node's levels from density on: each keeps the leaves of its run whose rank the density keeps, grown by the
        * square root of the run's area over theirs, its error how far they stray past their finest sides at the error
        * percentile (never under the level before's, from floor). Until one cluster is left. Then the sphere: centred on the run's feet's box,
        * holding every level's leaves and the children's spheres.
        */
        void makeLevels(std::uint32_t index, std::uint32_t density, float floor) {
            const std::uint32_t firstLeaf = ranked.brickFirstLeaf[far.nodes[index].firstBrick];
            const std::uint32_t endLeaf = ranked.brickFirstLeaf[far.nodes[index].firstBrick + far.nodes[index].brickCount];
            const auto count = static_cast<std::uint32_t>(ranked.leaves.size());
            float fullArea = 0.0f;
            Vec3 low = firstLeaf < endLeaf ? ranked.leaves[firstLeaf].foot : Vec3{};
            Vec3 high = low;

            for (std::uint32_t leaf = firstLeaf; leaf < endLeaf; ++leaf) {
                fullArea += ranked.area[leaf];
                const Vec3& foot = ranked.leaves[leaf].foot;
                low = Vec3{std::min(low.x, foot.x), std::min(low.y, foot.y), std::min(low.z, foot.z)};
                high = Vec3{std::max(high.x, foot.x), std::max(high.y, foot.y), std::max(high.z, foot.z)};
            }

            const Vec3 centre = (low + high) * 0.5f;
            float radius = 0.0f;
            far.nodes[index].firstLevel = static_cast<std::uint32_t>(far.levels.size());
            float error = floor;

            for (;; ++density) {
                const std::uint32_t keptRanks = prunedKept(count, static_cast<int>(density));
                float keptArea = 0.0f;
                std::vector<std::uint32_t> kept;
                std::vector<float> strays;

                for (std::uint32_t leaf = firstLeaf; leaf < endLeaf; ++leaf) {
                    if (ranked.rank[leaf] < keptRanks) {
                        kept.push_back(leaf);
                        keptArea += ranked.area[leaf];
                    }
                }

                TwigFarLevel& level = far.levels.emplace_back();
                level.firstLeaf = static_cast<std::uint32_t>(far.leaves.size());
                level.leafCount = static_cast<std::uint32_t>(kept.size());
                level.density = density;
                level.growth = density > 0 && keptArea > 0.0f ? std::sqrt(fullArea / keptArea) : 1.0f;

                for (const std::uint32_t leaf : kept) {
                    FarLeaf grown = ranked.leaves[leaf];
                    grown.side *= level.growth;
                    // The finest level's strays are its twigs' level's, its floor already.
                    if (density > 0) {
                        strays.push_back(grown.side - ranked.finestSide[leaf]);
                    }

                    radius = std::max(radius, (grown.foot - centre).length() + grown.side * ranked.reach[leaf]);
                    far.leaves.push_back(grown);
                }

                error = std::max(error, nearestRank(std::move(strays), errorShare));
                level.error = error;
                ++far.nodes[index].levelCount;

                if (kept.size() <= farLeavesPerCluster || keptRanks <= 1) {
                    break;
                }
            }

            for (const std::uint32_t child : far.nodes[index].children) {
                if (child != twigFarNoNode) {
                    radius = std::max(radius, (far.nodes[child].centre - centre).length() + far.nodes[child].radius);
                }
            }

            far.nodes[index].centre = centre;
            far.nodes[index].radius = radius;
        }
    };
}

FarLeaf farLeafOf(const TwigInstance& twig, std::uint32_t twigNumber, const TwigLeafPlace& leaf, float growth) {
    const auto turned = [&twig](const Vec3& local) {
        return twig.across * local.x + twig.towardsTip * local.y + twig.facing * local.z;
    };

    return FarLeaf{.foot = twigPoint(twig, leaf.foot),
                   .towardsTip = turned(leaf.towardsTip),
                   .facing = turned(leaf.facing),
                   .side = leaf.side * twig.scale * growth,
                   .cell = leaf.cell,
                   .number = leaf.number,
                   .twig = twigNumber};
}

TwigFarLevels makeTwigFarLevels(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, FarLeafShapes shapes, float errorPercentile) {
    if (!(errorPercentile > 0.0f && errorPercentile <= 100.0f)) {
        throw std::invalid_argument("A far error percentile must be in (0, 100]");
    }

    for (const TwigInstance& twig : set.twigs) {
        if (twig.prototype >= prototypes.size()) {
            throw std::invalid_argument("A twig's prototype hands over no far leaves");
        }

        for (const TwigLeafPlace& leaf : prototypes[twig.prototype].leaves) {
            if (leaf.cell >= shapes.cells.size()) {
                throw std::invalid_argument("A far leaf's cell has no shape");
            }
        }
    }

    TwigFarLevels far;
    far.shapes = std::move(shapes);

    if (set.bricks.empty()) {
        return far;
    }

    const RankedLeaves ranked = rankLeaves(set, prototypes);
    FarTreeBuilder(set, prototypes, ranked, errorPercentile / 100.0f, far).build(0, static_cast<std::uint32_t>(set.bricks.size()));
    return far;
}
