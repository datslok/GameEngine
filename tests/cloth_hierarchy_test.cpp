#include "cloth/cloth.h"
#include "cloth/cloth_collision.h"
#include "cloth/cloth_block_descent.h"
#include "cloth/cloth_constraints.h"
#include "cloth/cloth_energy.h"
#include "cloth/cloth_hierarchy.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    // Vertex block descent with this many coarse levels.
    ClothSettings blockDescent(int levels) {
        ClothSettings settings;
        settings.method = ClothMethod::VertexBlockDescent;
        settings.hierarchyLevels = levels;
        return settings;
    }

    // A yardstick sheet using every level it has.
    Cloth sheetWithLevels(int squares) {
        return yardstickSheet(squares, blockDescent(maxClothLevels));
    }

    // Where a point sits in its level's sorted list.
    std::size_t indexOnLevel(const ClothLevel& level, std::uint32_t point) {
        return static_cast<std::size_t>(std::lower_bound(level.points.begin(), level.points.end(), point) - level.points.begin());
    }

    // Every point of the level reached from its first along the level's mesh.
    bool levelIsConnected(const ClothLevel& level) {
        std::set<std::uint32_t> reached{level.points.front()};
        std::vector<std::uint32_t> waiting{level.points.front()};
        while (!waiting.empty()) {
            const std::size_t index = indexOnLevel(level, waiting.back());
            waiting.pop_back();
            for (std::uint32_t next = level.neighbourStarts[index]; next < level.neighbourStarts[index + 1]; ++next) {
                if (reached.insert(level.neighbours[next]).second) {
                    waiting.push_back(level.neighbours[next]);
                }
            }
        }
        return reached.size() == level.points.size();
    }

    ClothEnergyTerms energyTerms(const Cloth& cloth) {
        const ClothSettings& settings = cloth.settings;
        return ClothEnergyTerms{cloth.stretch,           cloth.bend,               cloth.pointConstraints, settings.stretchCompliance,
                                settings.bendingStiffness, settings.bendingModel, cloth.grab,            settings.grabCompliance};
    }

    float substepOf(const Cloth& cloth) {
        return tick / static_cast<float>(cloth.settings.substeps);
    }

    // The hierarchy's step alone, every level, from the cloth's prediction (its positions unless given).
    void coarseStep(Cloth& cloth, const std::vector<Shape>& bodies = {}, const std::vector<Vec3>* predicted = nullptr) {
        ClothHierarchyScratch scratch;
        scratch.predicted = predicted ? *predicted : cloth.particles.positions;
        solveClothHierarchy(cloth.particles, cloth.hierarchy, maxClothLevels, energyTerms(cloth), bodies, cloth.settings.thickness, substepOf(cloth),
                            scratch, serialJobs());
    }

    // A fixed run of pseudo-random numbers in -1..1, the same on every machine.
    struct Numbers {
        std::uint32_t state = 12345;
        float next() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
        }
        Vec3 nextVec() {
            const float x = next();
            const float y = next();
            return Vec3{x, y, next()};
        }
    };

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
    * On a grid, every fine point keeps at least clothParentsWanted coarse neighbours (its parents), a kept point (a pin)
    * stays coarse whatever its neighbours, a good share of the points go fine, and the same grid always gives the same
    * choice.
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
    * holding the pins, its mesh connected, each neighbour list sorted, on the level and two-way; every point not on a
    * level has at least clothParentsWanted parents on the level above its own, weighted to 1; each level's weights
    * from the cloth's points are as many one way as the other; each level's points have a mass.
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
            assert(level.neighbourStarts.size() == level.points.size() + 1 && level.gatherStarts.size() == level.points.size() + 1);
            assert(level.spreadStarts.size() == pointCount + 1);
            assert(level.masses.size() == level.points.size());
            for (const ClothPin& pin : cloth.pins) {
                assert(std::binary_search(level.points.begin(), level.points.end(), pin.particle));
            }

            for (std::size_t at = 0; at < level.points.size(); ++at) {
                const std::uint32_t point = level.points[at];
                assert(hierarchy.pointLevels[point] >= number);
                assert(level.masses[at] > 0.0f);
                const auto first = level.neighbours.begin() + level.neighbourStarts[at];
                const auto last = level.neighbours.begin() + level.neighbourStarts[at + 1];
                assert(first != last && std::is_sorted(first, last) && std::adjacent_find(first, last) == last);
                for (auto other = first; other != last; ++other) {
                    assert(*other != point && std::binary_search(level.points.begin(), level.points.end(), *other));
                    const std::size_t otherAt = indexOnLevel(level, *other);
                    assert(std::binary_search(level.neighbours.begin() + level.neighbourStarts[otherAt],
                                              level.neighbours.begin() + level.neighbourStarts[otherAt + 1], point));
                }
            }
            assert(levelIsConnected(level));

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
                for (std::uint32_t link = start; link < end; ++link) {
                    const ClothLink& parent = hierarchy.parents[link];
                    assert(std::binary_search(level.points.begin(), level.points.end(), parent.point));
                    assert(parent.weight > 0.0f && parent.weight <= 1.0f);
                    total += parent.weight;
                }
                assert(near(total, 1.0f, 1e-5f));
            }
            assert(level.spread.size() == level.gather.size());
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
        Cloth plain = yardstickSheet(6, blockDescent(0));
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
            assert(first.hierarchy.levels[index].neighbours == second.hierarchy.levels[index].neighbours);
            assert(first.hierarchy.levels[index].masses == second.hierarchy.levels[index].masses);
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
    * Spreading a level's values down passes on a shift exactly (the weights sum to 1 all the way down), and gathering up
    * is its transpose: for any g on the points and d on a level, (P^T g) . d = g . (P d), to rounding.
    */
    void testGatherIsTheTransposeOfSpread() {
        const Cloth cloth = sheetWithLevels(32);
        const ClothHierarchy& hierarchy = cloth.hierarchy;
        const std::size_t pointCount = cloth.particles.size();
        Numbers numbers;
        for (int number = 1; number <= static_cast<int>(hierarchy.levels.size()); ++number) {
            const ClothLevel& level = hierarchy.levels[static_cast<std::size_t>(number - 1)];

            std::vector<Vec3> shift(pointCount, Vec3{0.0f, 0.0f, 0.0f});
            for (const std::uint32_t point : level.points) {
                shift[point] = Vec3{0.25f, -1.0f, 2.0f};
            }
            prolongFromLevel(hierarchy, shift, number);
            for (const Vec3& value : shift) {
                assert(near(value.x, 0.25f, 1e-5f) && near(value.y, -1.0f, 1e-5f) && near(value.z, 2.0f, 1e-5f));
            }

            std::vector<Vec3> fine(pointCount);
            for (Vec3& value : fine) {
                value = numbers.nextVec();
            }
            std::vector<Vec3> coarse(pointCount, Vec3{0.0f, 0.0f, 0.0f});
            for (const std::uint32_t point : level.points) {
                coarse[point] = numbers.nextVec();
            }
            std::vector<Vec3> gathered = fine;
            restrictToLevel(hierarchy, gathered, number);
            std::vector<Vec3> spread = coarse;
            prolongFromLevel(hierarchy, spread, number);

            double up = 0.0;
            double down = 0.0;
            double size = 0.0;
            for (const std::uint32_t point : level.points) {
                up += gathered[point].dot(coarse[point]);
            }
            for (std::size_t point = 0; point < pointCount; ++point) {
                down += fine[point].dot(spread[point]);
                size += std::abs(fine[point].dot(spread[point]));
            }
            assert(std::abs(up - down) <= 1e-5 * size);
        }
    }

    // Each coarse point's mass is what its region weighs to its motion: the sum over the free points of the squared
    // share of its move each takes, times their mass (the diagonal of P^T M P), found here by spreading a unit move.
    void testRegionMasses() {
        const Cloth cloth = sheetWithLevels(24);
        const ClothHierarchy& hierarchy = cloth.hierarchy;
        const std::size_t pointCount = cloth.particles.size();
        for (int number = 1; number <= static_cast<int>(hierarchy.levels.size()); ++number) {
            const ClothLevel& level = hierarchy.levels[static_cast<std::size_t>(number - 1)];
            for (std::size_t at = 0; at < level.points.size(); at += 7) {
                std::vector<Vec3> move(pointCount, Vec3{0.0f, 0.0f, 0.0f});
                move[level.points[at]] = Vec3{1.0f, 0.0f, 0.0f};
                prolongFromLevel(hierarchy, move, number);
                double mass = 0.0;
                for (std::size_t point = 0; point < pointCount; ++point) {
                    const float inverseMass = cloth.particles.inverseMasses[point];
                    if (inverseMass > 0.0f) {
                        mass += double(move[point].x) * move[point].x / inverseMass;
                    }
                }
                assert(std::abs(level.masses[at] - mass) <= 1e-4 * mass);
            }
        }
    }

    /*
    * phi by hand: two points 1.5 m apart on an edge 1 m long (compliance 0.01 m/N: 0.5^2 / 0.02 = 12.5 J), the free one
    * (2 kg) 0.1 m from its prediction over a step of 0.1 s (2 0.01 / 0.02 = 1 J), held by a hand 0.2 m away (compliance
    * 0.1: 0.04 / 0.2 = 0.2 J). The pinned one's inertia does not count.
    */
    void testStepEnergy() {
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.5f, 0.0f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(2, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses = {0.0f, 0.5f};
        const std::vector<Vec3> predicted{Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.5f, 0.1f, 0.0f}};
        const std::vector<StretchConstraint> stretch{{0, 1, 1.0f}};
        const std::vector<BendConstraint> bend;
        const ClothPointConstraints lists = clothPointConstraints(stretch, bend, 2);
        const std::optional<ClothGrab> grab = ClothGrab{1, Vec3{1.5f, 0.2f, 0.0f}};
        const ClothEnergyTerms terms{stretch, bend, lists, 0.01f, 0.0f, BendingModel::Isometric, grab, 0.1f};
        assert(near(static_cast<float>(clothStepEnergy(particles, predicted, terms, 0.1f)), 13.7f, 1e-4f));
    }

    /*
    * The coarse step never raises phi: over a busy second (pins, a hand, a ball) and over a hammock settled with XPBD,
    * where Muller's one-sided limits raised it every substep (by 4.5 J on the hammock), phi after the step is never above
    * phi before it, between the sweeps of vertex block descent as the substep runs them. To rounding: after a sweep the
    * cloth is so near phi's minimum that what the step gains is micrometres, which float positions resolve only to about
    * a ten-millionth of a metre, so phi may come out up to a millionth higher (measured 1.4e-6 at most); allowed 1e-5.
    */
    void checkStepsLowerEnergy(Cloth cloth, const std::vector<Shape>& bodies) {
        const float substep = substepOf(cloth);
        for (int step = 0; step < 120 * cloth.settings.substeps; ++step) {
            predictCloth(cloth.particles, Vec3{0.0f, -cloth.settings.gravity, 0.0f}, substep);
            const std::vector<Vec3> predicted = cloth.particles.positions;
            solveVertexBlocks(cloth.particles, predicted, cloth.vertexColours, energyTerms(cloth), substep, serialJobs());
            const double before = clothStepEnergy(cloth.particles, predicted, energyTerms(cloth), substep);
            coarseStep(cloth, bodies, &predicted);
            assert(clothStepEnergy(cloth.particles, predicted, energyTerms(cloth), substep) <= before * (1.0 + 1e-5));
            solveVertexBlocks(cloth.particles, predicted, cloth.vertexColours, energyTerms(cloth), substep, serialJobs());
            solveContacts(cloth.particles, bodies, cloth.settings);
            finishClothStep(cloth.particles, substep);
        }
    }

    // A 2 m sheet of 40 x 40 squares pinned flat by its four corners at full span, 1.2 m up.
    Cloth hammock(const ClothSettings& settings) {
        Cloth cloth = makeCloth(Mesh::grid(40, 40, 2.0f, 2.0f), Mat4::translation(0.0f, 1.2f, 0.0f) * Mat4::rotationX(-0.5f * pi), settings);
        for (const std::uint32_t corner : {0u, 40u, 40u * 41u, 41u * 41u - 1u}) {
            addClothPin(cloth, corner);
        }
        return cloth;
    }

    void testCoarseStepLowersEnergy() {
        Cloth busy = makeCloth(Mesh::grid(40, 40, 1.5f, 1.5f), Mat4::translation(0.0f, 1.0f, 0.0f) * Mat4::rotationX(-0.5f * pi), ClothSettings{});
        addClothPin(busy, 40 * 41);
        addClothPin(busy, 40 * 41 + 40);
        busy.grab = ClothGrab{20, Vec3{0.3f, 0.8f, 0.6f}};
        checkStepsLowerEnergy(busy, {Shape{Sphere{Vec3{0.2f, 0.3f, 0.3f}, 0.35f}}});

        Cloth settled = hammock(ClothSettings{});
        for (int step = 0; step < 720; ++step) {
            stepCloth(settled, tick);
        }
        checkStepsLowerEnergy(settled, {});
    }

    // A weightless sheet at rest is at phi's minimum: the coarse step moves nothing (its flat hinges bend by rounding only).
    void testRestingClothStaysPut() {
        ClothSettings weightless = blockDescent(maxClothLevels);
        weightless.gravity = 0.0f;
        Cloth cloth = yardstickSheet(32, weightless);
        const std::vector<Vec3> before = cloth.particles.positions;
        coarseStep(cloth);
        for (std::size_t point = 0; point < before.size(); ++point) {
            assert((cloth.particles.positions[point] - before[point]).length() < 1e-6f);
        }
    }

    /*
    * Folding costs nothing without bending: a sheet folded in half along its middle row keeps every edge's length, so
    * with no bending stiffness it is at phi's minimum too, and stays (beyond the rounding of the folded positions).
    */
    void testFoldedSheetStaysPut() {
        Cloth cloth = sheetWithLevels(16);
        cloth.settings.bendingStiffness = 0.0f;
        const float fold = cloth.particles.positions[8 * 17].y;
        for (Vec3& position : cloth.particles.positions) {
            if (position.y < fold) {
                position = Vec3{position.x, 2.0f * fold - position.y, -position.z};
            }
        }
        const std::vector<Vec3> before = cloth.particles.positions;
        coarseStep(cloth);
        for (std::size_t point = 0; point < before.size(); ++point) {
            assert((cloth.particles.positions[point] - before[point]).length() < 1e-5f);
        }
    }

    /*
    * Held points never move: a sheet lying flat 3 mm above a box under its left half (within the cloth's 5 mm thickness)
    * and pinned at a corner of its right half, its prediction fallen by gravity. The points over the box and the pin
    * stay to the bit; the free ones move.
    */
    void testHeldPointsStay() {
        Cloth cloth = makeCloth(Mesh::grid(24, 24, 1.0f, 1.0f), Mat4::translation(0.0f, 0.003f, 0.0f) * Mat4::rotationX(-0.5f * pi),
                                blockDescent(maxClothLevels));
        pinClothPoint(cloth, 24);
        const std::vector<Shape> box{Shape{Aabb{Vec3{-1.0f, -0.5f, -1.0f}, Vec3{-0.01f, 0.0f, 1.0f}}}};
        const std::vector<Vec3> before = cloth.particles.positions;
        std::vector<Vec3> predicted = before;
        for (Vec3& position : predicted) {
            position.y -= 0.001f;
        }
        coarseStep(cloth, box, &predicted);

        bool freeMoved = false;
        for (std::size_t point = 0; point < before.size(); ++point) {
            const bool overBox = before[point].x < -0.02f;
            if (overBox || point == 24) {
                assert(std::memcmp(&cloth.particles.positions[point], &before[point], sizeof(Vec3)) == 0);
            } else if (std::memcmp(&cloth.particles.positions[point], &before[point], sizeof(Vec3)) != 0) {
                freeMoved = true;
            }
        }
        assert(freeMoved);
    }

    /*
    * The yardstick's problem turned round: settled with the levels, a sheet stretches well under half what it does with
    * XPBD or vertex block descent alone and hangs its sheet's height.
    */
    void testSettledSheetsWithLevels() {
        Cloth plain = yardstickSheet(32, ClothSettings{});
        Cloth single = yardstickSheet(32, blockDescent(0));
        Cloth layered = sheetWithLevels(32);
        JobSystem jobs;
        settleYardstick(plain, 4.0f, tick, jobs);
        settleYardstick(single, 4.0f, tick, jobs);
        settleYardstick(layered, 4.0f, tick, jobs);
        assert(measureSheet(layered, 32).pathStretch < 0.5f * measureSheet(plain, 32).pathStretch);
        assert(measureSheet(layered, 32).pathStretch < 0.5f * measureSheet(single, 32).pathStretch);
        assert(near(measureSheet(layered, 32).sag, 1.0f, 0.01f));
    }

    // The root-mean-square speed of the hammock over the second after it settles for 6 s: how much it still shakes.
    float hammockShaking(const ClothSettings& settings) {
        Cloth cloth = hammock(settings);
        JobSystem jobs;
        for (int step = 0; step < 720; ++step) {
            stepCloth(cloth, tick, {}, jobs);
        }
        double squares = 0.0;
        for (int step = 0; step < 120; ++step) {
            stepCloth(cloth, tick, {}, jobs);
            for (const Vec3& velocity : cloth.particles.velocities) {
                squares += velocity.lengthSquared();
            }
        }
        return static_cast<float>(std::sqrt(squares / (120.0 * cloth.particles.size())));
    }

    /*
    * A sagging sheet settles with the levels as it does with XPBD: with Muller's limits this hammock kept shaking at
    * about 50 times the speed (the user saw the cloth lab's jiggle), since they raised phi every substep; and a correct
    * coarse step still shook it 7 times as fast next to XPBD's pass, whose multipliers it moves the cloth without.
    */
    void testHammockSettles() {
        assert(hammockShaking(blockDescent(maxClothLevels)) < hammockShaking(ClothSettings{}));
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

    /*
    * A hand reaches the coarse levels through phi: pulling one edge of a floating sheet, the far edge follows further in
    * the first tick with the levels than without, the hand's spring part of the energy every level lowers.
    */
    void testHandReachesCoarseLevels() {
        const auto farEdgeMotion = [](int levels) {
            ClothSettings settings = blockDescent(levels);
            settings.gravity = 0.0f;
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

    // On one thread or eight, a busy sheet with the levels comes out the same to the last bit.
    void testManyThreadsMatchOne() {
        const auto run = [](JobSystem& jobs) {
            const ClothSettings settings = blockDescent(maxClothLevels);
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

    // XPBD without levels by default until the GPU solver has the rest too, so both step a cloth alike.
    void testOffByDefault() {
        assert(ClothSettings{}.method == ClothMethod::Xpbd);
        assert(ClothSettings{}.hierarchyLevels == 0);
    }
}


namespace {
    /*
    * The line search's curvature d . H d is a sum of terms the GPU adds one per thread (cloth.glsl mirrors each): a
    * point's inertia m |d|^2 / h^2, an edge's stiffness along it and (while stretched) across it, an isometric hinge's
    * stiffness |curvature of d|^2, a hinge's stiffness (its angle's change along d)^2, the hand's stiffness |d|^2.
    */
    void testCurvatureTerms() {
        assert(inertiaCurvature(Vec3{1.0f, 2.0f, 2.0f}, 100.0, 0.5f) == 1800.0);

        // An edge along x stretched 10%: moving its ends apart along it costs k along^2; across it, k (1 - r / L) across^2.
        const double stiffness = 2000.0;
        assert(near(static_cast<float>(edgeCurvature(Vec3{0.11f, 0.0f, 0.0f}, Vec3{0.01f, 0.0f, 0.0f}, 0.1f, stiffness)), 0.2f, 1e-5f));
        const double across = edgeCurvature(Vec3{0.11f, 0.0f, 0.0f}, Vec3{0.0f, 0.01f, 0.0f}, 0.1f, stiffness);
        assert(near(static_cast<float>(across), static_cast<float>(stiffness * (1.0 - 0.1 / 0.11) * 1e-4), 1e-6f));
        // Squashed, across costs nothing; too short to have a direction, nothing at all.
        assert(edgeCurvature(Vec3{0.09f, 0.0f, 0.0f}, Vec3{0.0f, 0.01f, 0.0f}, 0.1f, stiffness) == 0.0);
        assert(edgeCurvature(Vec3{}, Vec3{0.01f, 0.0f, 0.0f}, 0.1f, stiffness) == 0.0);

        const std::array<float, 4> weights{1.0f, 1.0f, -1.0f, -1.0f};
        const Vec3 up{0.0f, 1.0f, 0.0f};
        assert(isometricHingeCurvature(weights, up, up, Vec3{}, Vec3{}, 3.0) == 12.0);
        assert(isometricHingeCurvature(weights, up, up, up, up, 3.0) == 0.0);

        const std::array<Vec3, 4> slopes{Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, Vec3{}, Vec3{}};
        assert(hingeAngleCurvature(slopes, Vec3{1.0f, 0.0f, 0.0f}, up, Vec3{}, Vec3{}, 2.0) == 18.0);

        assert(handCurvature(Vec3{0.0f, 3.0f, 4.0f}, 10.0) == 250.0);
    }

    // The step along a direction: -slope / curvature, and nothing when it does not go downhill or phi is not curved up.
    void testLineSearchStep() {
        assert(lineSearchStep(-2.0, 4.0) == 0.5f);
        assert(lineSearchStep(0.0, 4.0) == 0.0f);
        assert(lineSearchStep(1.0, 4.0) == 0.0f);
        assert(lineSearchStep(-1.0, 0.0) == 0.0f);
        assert(lineSearchStep(-1.0, -1.0) == 0.0f);
    }

    /*
    * clothLineSearch is its terms summed: on a busy sheet along its gradient's descent, the step from the terms added
    * in a plain loop matches it (to double's rounding between the two orders of adding).
    */
    void testLineSearchIsItsTerms() {
        Cloth cloth = makeCloth(Mesh::grid(8, 8, 0.4f, 0.4f), Mat4::rotationX(-0.3f), blockDescent(maxClothLevels));
        addClothPin(cloth, 0);
        cloth.grab = ClothGrab{40, Vec3{0.1f, 0.3f, 0.2f}};
        std::vector<Vec3> predicted = cloth.particles.positions;
        for (std::size_t point = 0; point < predicted.size(); ++point) {
            predicted[point].y -= 0.0002f * float(point % 7);
        }
        const float substep = tick / 15.0f;
        const ClothEnergyTerms terms = energyTerms(cloth);
        std::vector<Vec3> gradient;
        clothEnergyGradient(cloth.particles, predicted, terms, substep, {}, gradient, serialJobs());
        std::vector<Vec3> direction(gradient.size());
        for (std::size_t point = 0; point < gradient.size(); ++point) {
            direction[point] = gradient[point] * -1e-7f;
        }

        const std::vector<Vec3>& positions = cloth.particles.positions;
        const double perSecondSquared = 1.0 / (double(substep) * substep);
        double slope = 0.0;
        double curvature = 0.0;
        for (std::size_t point = 0; point < positions.size(); ++point) {
            slope += double(gradient[point].dot(direction[point]));
            if (cloth.particles.inverseMasses[point] > 0.0f) {
                curvature += inertiaCurvature(direction[point], perSecondSquared, cloth.particles.inverseMasses[point]);
            }
        }
        for (const StretchConstraint& edge : cloth.stretch) {
            curvature += edgeCurvature(positions[edge.first] - positions[edge.second], direction[edge.first] - direction[edge.second], edge.restLength,
                                       energyStiffness(cloth.settings.stretchCompliance));
        }
        for (const BendConstraint& bend : cloth.bend) {
            curvature += isometricHingeCurvature(bend.isometricWeights, direction[bend.wing0], direction[bend.wing1], direction[bend.edgeStart],
                                                 direction[bend.edgeEnd], double(cloth.settings.bendingStiffness) * bend.stiffnessShare);
        }
        curvature += handCurvature(direction[cloth.grab->particle], energyStiffness(cloth.settings.grabCompliance));

        const float expected = lineSearchStep(slope, curvature);
        assert(expected > 0.0f);
        ClothParticles moved = cloth.particles;
        assert(near(clothLineSearch(moved, gradient, direction, terms, substep, serialJobs()), expected, 1e-6f * expected));
    }

    // A coarse edge stiffens its point along it by stiffness / length^2 times its outer product: stiffness along its direction.
    void testCoarseEdgeTerm() {
        ClothStiffnessBlock block;
        addCoarseEdgeTerm(block, Vec3{0.0f, 0.5f, 0.0f}, 100.0f);
        assert(block.yy == 100.0f && block.xx == 0.0f && block.zz == 0.0f && block.xy == 0.0f);
        addCoarseEdgeTerm(block, Vec3{}, 100.0f);
        assert(block.yy == 100.0f && block.xx == 0.0f);
    }
}

void testClothHierarchy() {
    testCoarsePointsOfAWheel();
    testCoarsePointsOfAGrid();
    testParentWeights();
    testHierarchyOfASheet();
    testSmallAndRepeatable();
    testPinnedPointOnEveryLevel();
    testGatherIsTheTransposeOfSpread();
    testRegionMasses();
    testStepEnergy();
    testCoarseStepLowersEnergy();
    testRestingClothStaysPut();
    testFoldedSheetStaysPut();
    testHeldPointsStay();
    testSettledSheetsWithLevels();
    testHammockSettles();
    testLevelsBeyondTheHierarchy();
    testHandReachesCoarseLevels();
    testManyThreadsMatchOne();
    testOffByDefault();
    testCurvatureTerms();
    testLineSearchStep();
    testLineSearchIsItsTerms();
    testCoarseEdgeTerm();
}
