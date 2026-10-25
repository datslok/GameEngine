#include "cloth/cloth.h"
#include "cloth/cloth_constraints.h"
#include "cloth/cloth_hierarchy.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <numbers>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace {
    constexpr float tick = 1.0f / 120.0f;
    constexpr float pi = std::numbers::pi_v<float>;

    // Each point's neighbours along the edges, sorted.
    std::vector<std::vector<std::uint32_t>> neighboursOf(const std::vector<StretchConstraint>& edges, std::size_t pointCount) {
        std::vector<std::vector<std::uint32_t>> neighbours(pointCount);
        for (const StretchConstraint& edge : edges) {
            neighbours[edge.first].push_back(edge.second);
            neighbours[edge.second].push_back(edge.first);
        }
        for (std::vector<std::uint32_t>& list : neighbours) {
            std::sort(list.begin(), list.end());
        }
        return neighbours;
    }

    std::vector<std::vector<std::uint32_t>> gridNeighbours(int squares) {
        return neighboursOf(stretchConstraints(Mesh::grid(squares, squares, 1.0f, 1.0f)), static_cast<std::size_t>((squares + 1) * (squares + 1)));
    }

    // A yardstick sheet using every level it has.
    Cloth sheetWithLevels(int squares) {
        ClothSettings settings;
        settings.hierarchyLevels = maxClothLevels;
        return yardstickSheet(squares, settings);
    }

    // Every point of the level reached from its first along the level's limits.
    bool levelIsConnected(const ClothLevel& level, std::size_t pointCount) {
        const std::vector<std::vector<std::uint32_t>> neighbours = neighboursOf(level.limits, pointCount);
        std::set<std::uint32_t> reached{level.points.front()};
        std::vector<std::uint32_t> waiting{level.points.front()};
        while (!waiting.empty()) {
            const std::uint32_t point = waiting.back();
            waiting.pop_back();
            for (std::uint32_t next : neighbours[point]) {
                if (reached.insert(next).second) {
                    waiting.push_back(next);
                }
            }
        }
        return reached.size() == level.points.size();
    }

    /*
    * The paper's greedy choice on a wheel: the hub, visited first, has six coarse neighbours and goes fine; then each
    * rim point has only two coarse neighbours left, too few to go fine. A point with fewer neighbours than
    * clothParentsWanted can never go fine, so a chain stays coarse.
    */
    void testCoarsePointsOfAWheel() {
        const std::vector<std::vector<std::uint32_t>> wheel{
            {1, 2, 3, 4, 5, 6}, {0, 2, 6}, {0, 1, 3}, {0, 2, 4}, {0, 3, 5}, {0, 4, 6}, {0, 1, 5}};
        const std::vector<bool> coarse = chooseCoarsePoints(wheel, std::vector<bool>(7, false));
        assert((coarse == std::vector<bool>{false, true, true, true, true, true, true}));

        const std::vector<std::vector<std::uint32_t>> chain{{1}, {0, 2}, {1, 3}, {2, 4}, {3}};
        assert(chooseCoarsePoints(chain, std::vector<bool>(5, false)) == std::vector<bool>(5, true));
    }

    /*
    * On a grid, every fine point keeps at least clothParentsWanted coarse neighbours (its parents), a kept point (a pin) stays coarse
    * whatever its neighbours, a good share of the points go fine, and the same grid always gives the same choice.
    */
    void testCoarsePointsOfAGrid() {
        const std::vector<std::vector<std::uint32_t>> neighbours = gridNeighbours(12);
        std::vector<bool> kept(neighbours.size(), false);
        kept[0] = true;
        kept[84] = true;
        const std::vector<bool> coarse = chooseCoarsePoints(neighbours, kept);

        std::size_t coarseCount = 0;
        for (std::size_t point = 0; point < neighbours.size(); ++point) {
            if (coarse[point]) {
                ++coarseCount;
                continue;
            }
            const auto coarseNeighbours = std::count_if(neighbours[point].begin(), neighbours[point].end(), [&](std::uint32_t next) {
                return coarse[next];
            });
            assert(coarseNeighbours >= clothParentsWanted);
        }
        assert(coarse[0] && coarse[84]);
        assert(coarseCount < neighbours.size() * 3 / 4);
        assert(chooseCoarsePoints(neighbours, kept) == coarse);
    }

    // Parents' weights sum to 1, the nearer parent weighing more (1 / (d / farthest d)), equal distances alike, and a
    // parent at the point itself taking nearly everything.
    void testParentWeights() {
        const std::vector<float> uneven = parentWeights(Vec3{0.0f, 0.0f, 0.0f}, {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 3.0f, 0.0f}});
        assert(near(uneven[0], 0.75f, 1e-3f) && near(uneven[1], 0.25f, 1e-3f));
        assert(near(uneven[0] + uneven[1], 1.0f, 1e-6f));

        const std::vector<float> even = parentWeights(Vec3{0.0f, 0.0f, 0.0f}, {Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -2.0f}, Vec3{0.0f, 2.0f, 0.0f}});
        for (const float weight : even) {
            assert(near(weight, 1.0f / 3.0f, 1e-6f));
        }

        const std::vector<float> onTop = parentWeights(Vec3{1.0f, 1.0f, 1.0f}, {Vec3{1.0f, 1.0f, 1.0f}, Vec3{2.0f, 1.0f, 1.0f}});
        assert(onTop[0] > 0.99f && near(onTop[0] + onTop[1], 1.0f, 1e-6f));
    }

    /*
    * The hierarchy of a pinned sheet: each level a smaller subset of the one below, no smaller than smallestClothLevel,
    * holding the pins, connected by its limits; each limit between two of its points, once, at their straight distance
    * in the rest shape, coloured; every point not on a level has at least clothParentsWanted parents on the level above
    * its own, weighted to 1, and each level lists those children.
    */
    void testHierarchyOfASheet() {
        const Cloth cloth = sheetWithLevels(32);
        const ClothHierarchy& hierarchy = cloth.hierarchy;
        const std::size_t pointCount = cloth.particles.size();
        assert(hierarchy.levels.size() >= 3 && hierarchy.levels.size() <= static_cast<std::size_t>(maxClothLevels));
        assert(hierarchy.pointLevels.size() == pointCount);
        assert(hierarchy.parentStarts.size() == pointCount + 1);

        std::vector<std::uint32_t> below(pointCount);
        for (std::uint32_t point = 0; point < pointCount; ++point) {
            below[point] = point;
        }
        for (std::size_t index = 0; index < hierarchy.levels.size(); ++index) {
            const ClothLevel& level = hierarchy.levels[index];
            const int number = static_cast<int>(index) + 1;
            assert(level.points.size() < below.size() && level.points.size() >= smallestClothLevel);
            assert(std::is_sorted(level.points.begin(), level.points.end()));
            assert(std::includes(below.begin(), below.end(), level.points.begin(), level.points.end()));
            for (const ClothPin& pin : cloth.pins) {
                assert(std::binary_search(level.points.begin(), level.points.end(), pin.particle));
            }
            for (const std::uint32_t point : level.points) {
                assert(hierarchy.pointLevels[point] >= number);
            }

            // The children: the points below not on this level, exactly those whose level is the one below.
            std::vector<std::uint32_t> children;
            std::set_difference(below.begin(), below.end(), level.points.begin(), level.points.end(), std::back_inserter(children));
            assert(level.children == children);
            for (const std::uint32_t child : children) {
                assert(hierarchy.pointLevels[child] == number - 1);
                const std::uint32_t start = hierarchy.parentStarts[child];
                const std::uint32_t end = hierarchy.parentStarts[child + 1];
                assert(end - start >= static_cast<std::uint32_t>(clothParentsWanted));
                float total = 0.0f;
                for (std::uint32_t parent = start; parent < end; ++parent) {
                    assert(std::binary_search(level.points.begin(), level.points.end(), hierarchy.parents[parent].point));
                    assert(hierarchy.parents[parent].weight > 0.0f && hierarchy.parents[parent].weight <= 1.0f);
                    total += hierarchy.parents[parent].weight;
                }
                assert(near(total, 1.0f, 1e-5f));
            }

            std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
            for (const StretchConstraint& limit : level.limits) {
                assert(limit.first < limit.second);
                assert(seen.insert({limit.first, limit.second}).second);
                assert(std::binary_search(level.points.begin(), level.points.end(), limit.first));
                assert(std::binary_search(level.points.begin(), level.points.end(), limit.second));
                assert(limit.restLength == (cloth.restPositions[limit.first] - cloth.restPositions[limit.second]).length());
            }
            assert(level.limitColours.front() == 0 && level.limitColours.back() == level.limits.size());
            for (std::size_t colour = 0; colour + 1 < level.limitColours.size(); ++colour) {
                std::set<std::uint32_t> used;
                for (std::uint32_t limit = level.limitColours[colour]; limit < level.limitColours[colour + 1]; ++limit) {
                    assert(used.insert(level.limits[limit].first).second && used.insert(level.limits[limit].second).second);
                }
            }
            assert(levelIsConnected(level, pointCount));
            below = level.points;
        }

        // The points on the coarsest level have no parents.
        for (const std::uint32_t point : hierarchy.levels.back().points) {
            assert(hierarchy.parentStarts[point] == hierarchy.parentStarts[point + 1]);
        }
    }

    // A small cloth gets no levels, and steps as it does without them; the same mesh always gets the same hierarchy.
    void testSmallAndRepeatable() {
        Cloth small = sheetWithLevels(6);
        assert(small.hierarchy.levels.empty());
        Cloth plain = yardstickSheet(6, ClothSettings{});
        for (int step = 0; step < 30; ++step) {
            stepCloth(small, tick);
            stepCloth(plain, tick);
        }
        assert(std::memcmp(small.particles.positions.data(), plain.particles.positions.data(), small.particles.size() * sizeof(Vec3)) == 0);

        const Cloth first = sheetWithLevels(24);
        const Cloth second = sheetWithLevels(24);
        assert(first.hierarchy.levels.size() == second.hierarchy.levels.size());
        for (std::size_t index = 0; index < first.hierarchy.levels.size(); ++index) {
            assert(first.hierarchy.levels[index].points == second.hierarchy.levels[index].points);
            assert(first.hierarchy.levels[index].limits.size() == second.hierarchy.levels[index].limits.size());
        }
        assert(first.hierarchy.parentStarts == second.hierarchy.parentStarts);
    }

    // A point pinned where it is, without a pin to move, is kept on every level too.
    void testPinnedPointOnEveryLevel() {
        Cloth cloth = sheetWithLevels(24);
        pinClothPoint(cloth, 12 * 25 + 12);
        assert(cloth.particles.inverseMasses[12 * 25 + 12] == 0.0f);
        assert(!cloth.hierarchy.levels.empty());
        for (const ClothLevel& level : cloth.hierarchy.levels) {
            assert(std::binary_search(level.points.begin(), level.points.end(), 12u * 25u + 12u));
        }
    }

    /*
    * A one-sided limit does nothing while its ends are within its length, and puts them back to it, exactly and in
    * proportion to their inverse masses, once they are further apart; a pinned end stays.
    */
    void testLimitsAreOneSided() {
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(3, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses = {0.0f, 1.0f, 1.0f};
        const std::vector<StretchConstraint> slack{{0, 1, 2.0f}};
        solveStretchLimits(particles, slack, {0, 1}, 0.0f, tick, serialJobs());
        assert(particles.positions[1].x == 1.0f);

        const std::vector<StretchConstraint> taut{{1, 2, 1.0f}};
        solveStretchLimits(particles, taut, {0, 1}, 0.0f, tick, serialJobs());
        assert(near(particles.positions[1].x, 2.0f, 1e-6f) && near(particles.positions[2].x, 3.0f, 1e-6f));

        const std::vector<StretchConstraint> fromPin{{0, 2, 2.0f}};
        solveStretchLimits(particles, fromPin, {0, 1}, 0.0f, tick, serialJobs());
        assert(particles.positions[0].x == 0.0f && near(particles.positions[2].x, 2.0f, 1e-6f));
    }

    // Steps of the hierarchy alone, from the coarsest level used down to the cloth's points.
    void hierarchyPass(Cloth& cloth, int levels, const std::optional<ClothGrab>& grab = std::nullopt) {
        std::vector<Vec3> starts(cloth.particles.size());
        solveClothHierarchy(cloth.particles, cloth.hierarchy, levels, 0.0f, grab, cloth.settings.grabCompliance, tick / 15.0f, starts, serialJobs());
    }

    // A sheet at rest is within every limit: the hierarchy moves nothing, to the last bit.
    void testSheetAtRestStaysPut() {
        Cloth cloth = sheetWithLevels(32);
        const std::vector<Vec3> before = cloth.particles.positions;
        hierarchyPass(cloth, maxClothLevels);
        assert(std::memcmp(before.data(), cloth.particles.positions.data(), before.size() * sizeof(Vec3)) == 0);
    }

    /*
    * Folding never pulls on the hierarchy: a sheet folded in half along its middle row keeps every edge's length, and
    * every coarse limit across the fold is slack, so nothing moves (beyond the rounding of the folded positions).
    */
    void testFoldedSheetStaysPut() {
        Cloth cloth = sheetWithLevels(16);
        const float fold = cloth.particles.positions[8 * 17].y;
        for (Vec3& position : cloth.particles.positions) {
            if (position.y < fold) {
                position = Vec3{position.x, 2.0f * fold - position.y, -position.z};
            }
        }
        const std::vector<Vec3> before = cloth.particles.positions;
        hierarchyPass(cloth, maxClothLevels);
        for (std::size_t point = 0; point < before.size(); ++point) {
            assert((cloth.particles.positions[point] - before[point]).length() < 1e-5f);
        }
    }

    /*
    * A sheet stretched 10% all the way down is pulled back further by a hierarchy pass and a fine pass than by the fine
    * pass alone, but only a little: children follow their parents' average motion, which passes on a shift exactly and
    * a stretch only roughly. The layer is for the small corrections of each substep, not a big stretch in one go.
    */
    void testHierarchyPullsAStretchedSheetBack() {
        constexpr int squares = 32;
        Cloth stretched = sheetWithLevels(squares);
        for (Vec3& position : stretched.particles.positions) {
            position.y *= 1.1f;
        }
        assert(near(measureSheet(stretched, squares).pathStretch, 0.1f, 1e-4f));

        Cloth plain = stretched;
        solveStretchColours(plain.particles, plain.stretch, plain.stretchColours, 0.0f, tick, serialJobs());
        Cloth layered = stretched;
        hierarchyPass(layered, maxClothLevels);
        solveStretchColours(layered.particles, layered.stretch, layered.stretchColours, 0.0f, tick, serialJobs());
        assert(measureSheet(layered, squares).pathStretch < measureSheet(plain, squares).pathStretch);
    }

    /*
    * The yardstick's problem turned round: settled with the hierarchy, a sheet stretches a small share of what it does
    * without (measured 0.035% against 1.5% at 32 squares), less even than a sheet half as fine without, and hangs its
    * sheet's height.
    */
    void testSettledSheetsWithLevels() {
        Cloth coarsePlain = yardstickSheet(16, ClothSettings{});
        Cloth finePlain = yardstickSheet(32, ClothSettings{});
        Cloth fine = sheetWithLevels(32);
        for (Cloth* cloth : {&coarsePlain, &finePlain, &fine}) {
            settleYardstick(*cloth, 4.0f, tick);
        }
        const float fineStretch = measureSheet(fine, 32).pathStretch;
        assert(fineStretch < 0.1f * measureSheet(finePlain, 32).pathStretch);
        assert(fineStretch < measureSheet(coarsePlain, 16).pathStretch);
        assert(near(measureSheet(fine, 32).sag, 1.0f, 0.005f));
    }

    // Asking for more levels than a cloth has uses all it has: the same result to the last bit.
    void testLevelsBeyondTheHierarchy() {
        Cloth all = sheetWithLevels(24);
        Cloth beyond = sheetWithLevels(24);
        all.settings.hierarchyLevels = static_cast<int>(all.hierarchy.levels.size());
        beyond.settings.hierarchyLevels = maxClothLevels;
        for (int step = 0; step < 30; ++step) {
            stepCloth(all, tick);
            stepCloth(beyond, tick);
        }
        assert(std::memcmp(all.particles.positions.data(), beyond.particles.positions.data(), all.particles.size() * sizeof(Vec3)) == 0);
    }

    // The point standing for a held point on a level: itself if it is on that level, else one on it, reached by its
    // heaviest parents.
    void testPointOnLevel() {
        const Cloth cloth = sheetWithLevels(16);
        const ClothHierarchy& hierarchy = cloth.hierarchy;
        for (std::uint32_t point = 0; point < cloth.particles.size(); ++point) {
            for (int level = 0; level <= static_cast<int>(hierarchy.levels.size()); ++level) {
                const std::uint32_t standIn = pointOnLevel(hierarchy, point, level);
                assert(hierarchy.pointLevels[standIn] >= level);
                if (hierarchy.pointLevels[point] >= level) {
                    assert(standIn == point);
                }
            }
        }
    }

    /*
    * A hand reaches the coarse levels too: pulling one edge of a floating sheet, the far edge follows further in the
    * first tick with the hierarchy than without, the pull carried across by the coarse points the hand's point stands in
    * for.
    */
    void testHandReachesCoarseLevels() {
        const auto farEdgeMotion = [](int levels) {
            ClothSettings settings;
            settings.gravity = 0.0f;
            settings.hierarchyLevels = levels;
            Cloth cloth = makeCloth(Mesh::grid(24, 24, 1.0f, 1.0f), Mat4::identity(), settings);
            const std::uint32_t held = 12;
            const std::uint32_t far = 24 * 25 + 12;
            const Vec3 farStart = cloth.particles.positions[far];
            cloth.grab = ClothGrab{held, cloth.particles.positions[held] + Vec3{0.0f, -0.2f, 0.0f}};
            stepCloth(cloth, tick);
            return (cloth.particles.positions[far] - farStart).length();
        };
        assert(farEdgeMotion(maxClothLevels) > 2.0f * farEdgeMotion(0));
    }

    // On one thread or eight, a busy sheet with the hierarchy comes out the same to the last bit.
    void testManyThreadsMatchOne() {
        const auto run = [](JobSystem& jobs) {
            ClothSettings settings;
            settings.hierarchyLevels = maxClothLevels;
            Cloth cloth = makeCloth(Mesh::grid(40, 40, 1.5f, 1.5f), Mat4::translation(0.0f, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), settings);
            addClothPin(cloth, 40 * 41);
            addClothPin(cloth, 40 * 41 + 40);
            const std::vector<Shape> ball{Shape{Sphere{Vec3{0.2f, 0.3f, 0.3f}, 0.35f}}};
            for (int step = 0; step < 60; ++step) {
                cloth.grab = ClothGrab{20, Vec3{0.3f, 0.8f + 0.001f * step, 0.6f}};
                stepCloth(cloth, tick, ball, jobs);
            }
            return cloth.particles.positions;
        };
        JobSystem serial{0};
        JobSystem parallel{7};
        const std::vector<Vec3> one = run(serial);
        const std::vector<Vec3> many = run(parallel);
        assert(std::memcmp(one.data(), many.data(), one.size() * sizeof(Vec3)) == 0);
    }

    // Off by default until the GPU solver has it too, so both solvers step a cloth alike.
    void testOffByDefault() {
        assert(ClothSettings{}.hierarchyLevels == 0);
    }
}

void testClothHierarchy() {
    testCoarsePointsOfAWheel();
    testCoarsePointsOfAGrid();
    testParentWeights();
    testHierarchyOfASheet();
    testSmallAndRepeatable();
    testPinnedPointOnEveryLevel();
    testLimitsAreOneSided();
    testSheetAtRestStaysPut();
    testFoldedSheetStaysPut();
    testHierarchyPullsAStretchedSheetBack();
    testSettledSheetsWithLevels();
    testLevelsBeyondTheHierarchy();
    testPointOnLevel();
    testHandReachesCoarseLevels();
    testManyThreadsMatchOne();
    testOffByDefault();
}
