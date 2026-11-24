#include "scene/twig_far_levels.h"

#include "math/percentile.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "test_levels.h"
#include "twig_helpers.h"

namespace {
    TwigSet testSet(std::size_t twigs) {
        return makeTwigSet({MeshHandle{3}, MeshHandle{4}, MeshHandle{5}}, {1.2f, 1.0f, 0.8f}, scatteredTwigs(twigs));
    }

    // The handed-over leaf a record came from: its twig's prototype's leaf of its number.
    const TwigLeafPlace& sourceOf(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, const FarLeaf& leaf) {
        for (const TwigLeafPlace& place : prototypes[set.twigs[leaf.twig].prototype].leaves) {
            if (place.number == leaf.number) {
                return place;
            }
        }

        assert(false);
        return prototypes.front().leaves.front();
    }

    // A level's leaves as (twig, number), the leaf's identity across levels.
    std::set<std::pair<std::uint32_t, std::uint32_t>> leavesOf(const TwigFarLevels& far, const TwigFarLevel& level) {
        std::set<std::pair<std::uint32_t, std::uint32_t>> leaves;

        for (std::uint32_t leaf = level.firstLeaf; leaf < level.firstLeaf + level.leafCount; ++leaf) {
            leaves.insert({far.leaves[leaf].twig, far.leaves[leaf].number});
        }

        return leaves;
    }

    // Two records alike in every field, exactly.
    bool sameFarLeaf(const FarLeaf& a, const FarLeaf& b) {
        const auto same = [](const Vec3& p, const Vec3& q) {
            return p.x == q.x && p.y == q.y && p.z == q.z;
        };

        return same(a.foot, b.foot) && same(a.towardsTip, b.towardsTip) && same(a.facing, b.facing) && a.side == b.side && a.cell == b.cell &&
               a.number == b.number && a.twig == b.twig;
    }

    // A leaf's area in the plant's frame: its shape's area times its side squared.
    float areaOf(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, const FarLeaf& leaf) {
        return sourceOf(set, prototypes, leaf).area * leaf.side * leaf.side;
    }

    /*
    * A record is its leaf carried by its twig: foot and frame placed, side times the twig's scale and the growth. A
    * brick's finest level holds exactly its twigs' handed-over leaves, in twig order, grown by nothing more.
    */
    void testFarLeavesAreTheTwigsLeaves() {
        const TwigInstance twig{.foot = Vec3{1.0f, 2.0f, 3.0f},
                                .across = Vec3{0.0f, 0.0f, -1.0f},
                                .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                                .facing = Vec3{1.0f, 0.0f, 0.0f},
                                .scale = 2.0f,
                                .lean = Vec3{}};
        const TwigLeafPlace place{.foot = Vec3{1.0f, 0.5f, 0.25f}, .towardsTip = Vec3{1.0f, 0.0f, 0.0f}, .facing = Vec3{0.0f, 0.0f, 1.0f},
                                  .side = 0.1f, .cell = 7, .number = 9};
        const FarLeaf carried = farLeafOf(twig, 42, place, 1.5f);
        assert((carried.foot - Vec3{1.5f, 3.0f, 1.0f}).length() < 1e-6f);
        assert((carried.towardsTip - Vec3{0.0f, 0.0f, -1.0f}).length() < 1e-6f && (carried.facing - Vec3{1.0f, 0.0f, 0.0f}).length() < 1e-6f);
        assert(near(carried.side, 0.3f, 1e-6f) && carried.cell == 7 && carried.number == 9 && carried.twig == 42);

        const std::vector<TwigFarLeaves> prototypes = standInFarLeaves();
        const TwigSet set = testSet(2000);
        const TwigFarLevels far = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);
        assert(far.nodes.size() > set.bricks.size());

        for (std::size_t brick = 0; brick < set.bricks.size(); ++brick) {
            const TwigFarNode& node = far.nodes[brick];
            assert(node.firstBrick == brick && node.brickCount == 1 && node.children[0] == twigFarNoNode && node.levelCount > 0);
            const TwigFarLevel& finest = far.levels[node.firstLevel];
            assert(finest.density == 0 && finest.growth == 1.0f);
            std::uint32_t next = finest.firstLeaf;

            for (std::uint32_t number = set.bricks[brick].firstTwig; number < set.bricks[brick].firstTwig + set.bricks[brick].twigCount; ++number) {
                const TwigInstance& placed = set.twigs[number];

                for (const TwigLeafPlace& leaf : prototypes[placed.prototype].leaves) {
                    assert(sameFarLeaf(far.leaves[next], farLeafOf(placed, number, leaf, 1.0f)));
                    ++next;
                }
            }

            assert(next == finest.firstLeaf + finest.leafCount);
        }
    }

    /*
    * Each level keeps about half the leaves of the one before, a subset of them, grown so the node keeps the area of
    * all its leaves; a merged node's first level is a subset of its children's last levels; levels shrink until one
    * cluster is left (only the last fits one); every record of a level carries its growth.
    */
    void testFarLevelsNestAndKeepTheArea() {
        const std::vector<TwigFarLeaves> prototypes = standInFarLeaves();
        const TwigSet set = testSet(3000);
        const TwigFarLevels far = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);

        // Each node's full area: its bricks' finest levels'.
        std::vector<float> fullArea(far.nodes.size(), 0.0f);

        for (std::size_t index = 0; index < far.nodes.size(); ++index) {
            const TwigFarNode& node = far.nodes[index];

            for (std::uint32_t brick = node.firstBrick; brick < node.firstBrick + node.brickCount; ++brick) {
                const TwigFarLevel& finest = far.levels[far.nodes[brick].firstLevel];

                for (std::uint32_t leaf = finest.firstLeaf; leaf < finest.firstLeaf + finest.leafCount; ++leaf) {
                    fullArea[index] += areaOf(set, prototypes, far.leaves[leaf]);
                }
            }
        }

        for (std::size_t index = 0; index < far.nodes.size(); ++index) {
            const TwigFarNode& node = far.nodes[index];
            std::set<std::pair<std::uint32_t, std::uint32_t>> before;

            if (node.children[0] != twigFarNoNode) {
                for (const std::uint32_t child : node.children) {
                    const TwigFarNode& made = far.nodes[child];
                    assert(made.parent == index);
                    const auto last = leavesOf(far, far.levels[made.firstLevel + made.levelCount - 1]);
                    before.insert(last.begin(), last.end());
                }
            }

            for (std::uint32_t level = node.firstLevel; level < node.firstLevel + node.levelCount; ++level) {
                const TwigFarLevel& made = far.levels[level];
                const auto kept = leavesOf(far, made);
                assert(kept.size() == made.leafCount);
                const bool last = level + 1 == node.firstLevel + node.levelCount;
                assert(last ? made.leafCount <= farLeavesPerCluster : made.leafCount > farLeavesPerCluster);

                if (!before.empty()) {
                    assert(std::includes(before.begin(), before.end(), kept.begin(), kept.end()));
                    assert(kept.size() * 2 <= before.size() + 2 * node.brickCount);
                }

                float area = 0.0f;

                for (std::uint32_t leaf = made.firstLeaf; leaf < made.firstLeaf + made.leafCount; ++leaf) {
                    const FarLeaf& record = far.leaves[leaf];
                    const TwigInstance& twig = set.twigs[record.twig];
                    assert(near(record.side, sourceOf(set, prototypes, record).side * twig.scale * made.growth, 1e-5f));
                    area += areaOf(set, prototypes, record);
                }

                if (made.leafCount > 0) {
                    assert(std::abs(area - fullArea[index]) <= 1e-3f * fullArea[index]);
                }

                if (level > node.firstLevel) {
                    assert(made.density == far.levels[level - 1].density + 1);
                }

                before = kept;
            }
        }

        // The root holds every brick, one cluster at last.
        const TwigFarNode& root = far.nodes.back();
        assert(root.parent == twigFarNoNode && root.firstBrick == 0 && root.brickCount == set.bricks.size());
        assert(far.levels[root.firstLevel + root.levelCount - 1].leafCount <= farLeavesPerCluster);
    }

    /*
    * Errors never fall: along a node's levels, from children to parent, and a brick's finest is its twigs' (the
    * twig level's error times the twig's scale). Each is at least how far each of its grown leaves strays past its
    * finest size.
    */
    void testFarErrorsRise() {
        const std::vector<TwigFarLeaves> prototypes = standInFarLeaves();
        const TwigSet set = testSet(1500);
        const TwigFarLevels far = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);

        for (std::size_t index = 0; index < far.nodes.size(); ++index) {
            const TwigFarNode& node = far.nodes[index];
            float floor = 0.0f;

            if (node.children[0] == twigFarNoNode) {
                for (std::uint32_t twig = set.bricks[index].firstTwig; twig < set.bricks[index].firstTwig + set.bricks[index].twigCount; ++twig) {
                    floor = std::max(floor, prototypes[set.twigs[twig].prototype].error * set.twigs[twig].scale);
                }

                assert(far.levels[node.firstLevel].error == floor);
            } else {
                for (const std::uint32_t child : node.children) {
                    const TwigFarNode& made = far.nodes[child];
                    floor = std::max(floor, far.levels[made.firstLevel + made.levelCount - 1].error);
                }
            }

            for (std::uint32_t level = node.firstLevel; level < node.firstLevel + node.levelCount; ++level) {
                const TwigFarLevel& made = far.levels[level];
                assert(made.error >= floor);

                for (std::uint32_t leaf = made.firstLeaf; leaf < made.firstLeaf + made.leafCount; ++leaf) {
                    const FarLeaf& record = far.leaves[leaf];
                    const TwigInstance& twig = set.twigs[record.twig];
                    const float finest = sourceOf(set, prototypes, record).side * twig.scale / prototypes[twig.prototype].growth;
                    assert(made.error >= record.side - finest - 1e-6f);
                }

                floor = made.error;
            }

            // The coarser levels grow: the last's error past the finest's.
            if (node.levelCount > 1) {
                assert(far.levels[node.firstLevel + node.levelCount - 1].error > far.levels[node.firstLevel].error);
            }
        }
    }

    /*
    * The tree merges the bricks as their median split did (each merged node its children's bricks, the first child
    * twigBricksFirstHalf of them), and each node's sphere holds its leaves at every level (foot plus side times reach)
    * and its children's spheres.
    */
    void testFarTreeFollowsTheSplit() {
        const std::vector<TwigFarLeaves> prototypes = standInFarLeaves();
        const TwigSet set = testSet(2500);
        const TwigFarLevels far = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);
        std::size_t roots = 0;

        for (std::size_t index = 0; index < far.nodes.size(); ++index) {
            const TwigFarNode& node = far.nodes[index];
            roots += node.parent == twigFarNoNode ? 1 : 0;
            assert(node.parent == twigFarNoNode || node.parent > index);

            if (node.children[0] != twigFarNoNode) {
                const TwigFarNode& first = far.nodes[node.children[0]];
                const TwigFarNode& second = far.nodes[node.children[1]];
                assert(index >= set.bricks.size() && node.children[0] < index && node.children[1] < index);
                assert(first.firstBrick == node.firstBrick && first.brickCount == twigBricksFirstHalf(node.brickCount));
                assert(second.firstBrick == first.firstBrick + first.brickCount && first.brickCount + second.brickCount == node.brickCount);

                for (const TwigFarNode* child : {&first, &second}) {
                    assert((child->centre - node.centre).length() + child->radius <= node.radius * 1.0001f + 1e-5f);
                }
            }

            for (std::uint32_t level = node.firstLevel; level < node.firstLevel + node.levelCount; ++level) {
                const TwigFarLevel& made = far.levels[level];

                for (std::uint32_t leaf = made.firstLeaf; leaf < made.firstLeaf + made.leafCount; ++leaf) {
                    const FarLeaf& record = far.leaves[leaf];
                    const float reach = record.side * sourceOf(set, prototypes, record).reach;
                    assert((record.foot - node.centre).length() + reach <= node.radius * 1.0001f + 1e-5f);
                }
            }
        }

        assert(roots == 1 && far.nodes.size() == 2 * set.bricks.size() - 1);

        // The same set makes the same levels; a prototype not handed over is refused; no twigs, nothing.
        const TwigFarLevels again = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);
        assert(again.leaves.size() == far.leaves.size() && again.levels.size() == far.levels.size());

        for (std::size_t leaf = 0; leaf < far.leaves.size(); ++leaf) {
            assert(sameFarLeaf(again.leaves[leaf], far.leaves[leaf]));
        }
        assert(throwsInvalid([&] { makeTwigFarLevels(set, {prototypes[0], prototypes[1]}, standInFarShapes(), farErrorWorstPercentile); }));
        FarLeafShapes fewer = standInFarShapes();
        fewer.cells.resize(10);
        assert(throwsInvalid([&] { makeTwigFarLevels(set, prototypes, fewer, farErrorWorstPercentile); }));
        assert(far.shapes.cells.size() == 24 && far.shapes.curl == 0.08f);
        const TwigFarLevels none = makeTwigFarLevels(makeTwigSet({}, {}, {}), prototypes, standInFarShapes(), farErrorWorstPercentile);
        assert(none.leaves.empty() && none.levels.empty() && none.nodes.empty());
    }

    /*
    * The nearest rank: the smallest value with at least the share of the values at or under it, so share 1 is the
    * largest; nothing given, 0; a share outside (0, 1] refused.
    */
    void testNearestRank() {
        const std::vector<float> values{5.0f, 1.0f, 4.0f, 2.0f, 3.0f};
        assert(nearestRank(values, 1.0f) == 5.0f);
        assert(nearestRank(values, 0.5f) == 3.0f);
        assert(nearestRank(values, 0.6f) == 3.0f && nearestRank(values, 0.61f) == 4.0f);
        assert(nearestRank(values, 0.2f) == 1.0f && nearestRank(values, 0.01f) == 1.0f);
        assert(nearestRank({}, 0.5f) == 0.0f);
        assert(throwsInvalid([&] { nearestRank(values, 0.0f); }));
        assert(throwsInvalid([&] { nearestRank(values, 1.005f); }));
        assert(throwsInvalid([&] { nearestRank(values, std::nanf("")); }));
    }

    /*
    * Below 100, a level's error is its typical leaf's (ROADMAP phase 21 stage 3e-iv): a brick's finest the percentile
    * of its leaves' twig errors (the twig level's times the twig's scale, once a leaf), each coarser level the
    * percentile of its grown leaves' strays, never under the level before's, a merged node's floor its children's last.
    * The leaves and levels are the worst leaf's exactly; only errors fall, and some do.
    */
    void testFarErrorsFromATypicalLeaf() {
        const std::vector<TwigFarLeaves> prototypes = standInFarLeaves();
        const TwigSet set = testSet(1500);
        const float percentile = 50.0f;
        const TwigFarLevels worst = makeTwigFarLevels(set, prototypes, standInFarShapes(), farErrorWorstPercentile);
        const TwigFarLevels typical = makeTwigFarLevels(set, prototypes, standInFarShapes(), percentile);
        assert(typical.leaves.size() == worst.leaves.size() && typical.levels.size() == worst.levels.size());

        for (std::size_t leaf = 0; leaf < worst.leaves.size(); ++leaf) {
            assert(sameFarLeaf(typical.leaves[leaf], worst.leaves[leaf]));
        }

        std::size_t lower = 0;

        for (std::size_t index = 0; index < typical.nodes.size(); ++index) {
            const TwigFarNode& node = typical.nodes[index];
            float floor = 0.0f;

            if (node.children[0] == twigFarNoNode) {
                std::vector<float> twigErrors;

                for (std::uint32_t twig = set.bricks[index].firstTwig; twig < set.bricks[index].firstTwig + set.bricks[index].twigCount; ++twig) {
                    const TwigFarLeaves& handed = prototypes[set.twigs[twig].prototype];
                    twigErrors.insert(twigErrors.end(), handed.leaves.size(), handed.error * set.twigs[twig].scale);
                }

                floor = nearestRank(twigErrors, percentile / 100.0f);
            } else {
                for (const std::uint32_t child : node.children) {
                    const TwigFarNode& made = typical.nodes[child];
                    floor = std::max(floor, typical.levels[made.firstLevel + made.levelCount - 1].error);
                }
            }

            for (std::uint32_t level = node.firstLevel; level < node.firstLevel + node.levelCount; ++level) {
                const TwigFarLevel& made = typical.levels[level];
                std::vector<float> strays;

                for (std::uint32_t leaf = made.firstLeaf; leaf < made.firstLeaf + made.leafCount && made.density > 0; ++leaf) {
                    const FarLeaf& record = typical.leaves[leaf];
                    const TwigInstance& twig = set.twigs[record.twig];
                    strays.push_back(record.side - sourceOf(set, prototypes, record).side * twig.scale / prototypes[twig.prototype].growth);
                }

                floor = std::max(floor, nearestRank(strays, percentile / 100.0f));
                assert(near(made.error, floor, 1e-6f));
                assert(made.error <= worst.levels[level].error);
                lower += made.error < worst.levels[level].error * 0.99f ? 1 : 0;
            }
        }

        assert(lower * 2 > typical.levels.size());
        assert(throwsInvalid([&] { makeTwigFarLevels(set, prototypes, standInFarShapes(), 0.0f); }));
        assert(throwsInvalid([&] { makeTwigFarLevels(makeTwigSet({}, {}, {}), prototypes, standInFarShapes(), 101.0f); }));
    }
}

void testTwigFarLevels() {
    testFarLeavesAreTheTwigsLeaves();
    testFarLevelsNestAndKeepTheArea();
    testFarErrorsRise();
    testFarTreeFollowsTheSplit();
    testNearestRank();
    testFarErrorsFromATypicalLeaf();
}
