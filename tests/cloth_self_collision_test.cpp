#include "cloth/cloth.h"
#include "cloth/cloth_self_collision.h"
#include "engine/settings_file.h"
#include "math/closest_points.h"

#include "cloth_helpers.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance = 1e-6f) {
        return std::fabs(a - b) <= tolerance;
    }

    // The first point at height, the triangle (a, b, c) level at y = 0 under it: the plane's tests' pair.
    struct PointOverTriangle {
        std::vector<Vec3> positions{Vec3{0.02f, 0.008f, 0.02f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.1f}};
        ClothSelfPair pair = pointTrianglePair(positions, 0, {1, 2, 3});
    };

    std::array<Vec3, 4> moves(const Vec3& point, const Vec3& triangle) {
        return {point, triangle, triangle, triangle};
    }

    // A flat grid's triangles and rest positions, as a cloth keeps them.
    struct FlatGrid {
        std::vector<Vec3> positions;
        std::vector<std::array<std::uint32_t, 3>> triangles;

        FlatGrid(int squares, float size) {
            const Mesh grid = Mesh::grid(squares, squares, size, size);
            for (const Vec4& vertex : grid.vertices) {
                positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
            }
            for (const Triangle& triangle : grid.triangles) {
                triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                     static_cast<std::uint32_t>(triangle.third)});
            }
        }
    };

    // Points count as one when stitched, and near when one edge apart.
    void testNeighbourhood() {
        const FlatGrid grid{2, 1.0f}; // points row * 3 + column
        const std::vector<std::array<std::uint32_t, 3>>& triangles = grid.triangles;

        const ClothSelfTopology plain = clothSelfTopology(grid.positions, triangles, {});
        assert(nearInCloth(plain, 0, 0));
        assert(nearInCloth(plain, 0, 1));
        assert(nearInCloth(plain, 1, 0));
        assert(!nearInCloth(plain, 0, 2));
        assert(!nearInCloth(plain, 0, 8));
        assert(plain.edges.size() == 16);

        const ClothSelfTopology sewn = clothSelfTopology(grid.positions, triangles, {ClothStitch{0, 8}});
        assert(sewn.joined[8] == sewn.joined[0]);
        assert(nearInCloth(sewn, 0, 8));
        assert(nearInCloth(sewn, 0, 7));
        assert(nearInCloth(sewn, 5, 0));
        assert(!nearInCloth(sewn, 0, 2));
    }

    /*
    * A flat sheet is left alone: on a 4 mm mesh, finer than the 5 mm gap, the neighbours already that close at rest are
    * no contacts, and on a coarse sheet none is within reach. Neighbours brought close later are, which is what keeps a
    * fold from passing through the square beside it.
    */
    void testFlatSheetsAreLeftAlone() {
        const float thickness = 0.005f;
        const FlatGrid fine{10, 0.04f};
        const ClothSelfTopology fineTopology = clothSelfTopology(fine.positions, fine.triangles, {});
        for (const ClothSelfPair& pair : findClothPairs(fine.positions, fine.triangles, fineTopology, 0.01f, thickness)) {
            assert(pair.gap >= thickness);
        }

        const FlatGrid coarse{2, 0.1f};
        const ClothSelfTopology coarseTopology = clothSelfTopology(coarse.positions, coarse.triangles, {});
        assert(findClothPairs(coarse.positions, coarse.triangles, coarseTopology, 0.01f, thickness).empty());

        // Point 0 folded down onto the triangle beside its own, 3 mm over it: a contact although they are neighbours.
        std::vector<Vec3> folded = coarse.positions;
        bool found = false;
        for (const std::array<std::uint32_t, 3>& triangle : coarse.triangles) {
            const bool own = triangle[0] == 0 || triangle[1] == 0 || triangle[2] == 0;
            if (own || !(nearInCloth(coarseTopology, 0, triangle[0]) || nearInCloth(coarseTopology, 0, triangle[1]) ||
                         nearInCloth(coarseTopology, 0, triangle[2]))) {
                continue;
            }
            const Vec3 centre = (coarse.positions[triangle[0]] + coarse.positions[triangle[1]] + coarse.positions[triangle[2]]) * (1.0f / 3.0f);
            folded[0] = centre + Vec3{0.0f, 0.0f, 0.003f};
            for (const ClothSelfPair& pair : findClothPairs(folded, coarse.triangles, coarseTopology, 0.01f, thickness)) {
                found = found || (pair.kind == ClothPairKind::PointTriangle && pair.points[0] == 0 && pair.points[1] == triangle[0] &&
                                  pair.points[2] == triangle[1] && pair.points[3] == triangle[2]);
            }
            break;
        }
        assert(found);
    }

    // Pairs are found between layers within reach, never within a layer's neighbourhoods, and none past the reach.
    void testPairsAreFound() {
        const Mesh sheets = stackedSheets(2, 0.1f, 0.008f);
        const Cloth cloth = makeCloth(sheets, Mat4::identity(), ClothSettings{});
        const std::vector<Vec3>& positions = cloth.particles.positions;

        const std::vector<ClothSelfPair> pairs = findClothPairs(positions, cloth.triangles, cloth.selfTopology, 0.01f, cloth.settings.thickness);
        int pointTriangle = 0;
        int edgeEdge = 0;
        for (const ClothSelfPair& pair : pairs) {
            (pair.kind == ClothPairKind::PointTriangle ? pointTriangle : edgeEdge) += 1;
            const bool firstUpper = positions[pair.points[0]].y > 0.004f;
            const bool secondUpper = positions[pair.points[3]].y > 0.004f;
            assert(firstUpper != secondUpper);
            assert(near(pair.gap, 0.008f, 1e-5f));
        }
        assert(pointTriangle > 0);
        assert(edgeEdge > 0);

        assert(findClothPairs(positions, cloth.triangles, cloth.selfTopology, 0.005f, cloth.settings.thickness).empty());

        // The same cloth gives the same pairs in the same order.
        const std::vector<ClothSelfPair> again = findClothPairs(positions, cloth.triangles, cloth.selfTopology, 0.01f, cloth.settings.thickness);
        assert(again.size() == pairs.size());
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            assert(again[index].points == pairs[index].points);
        }
    }

    /*
    * Planar-DAT's plane (Chen et al. 2026, Eq. 9-11) gives each side room in proportion to how fast it approaches: a
    * point falling onto a still triangle gets all of it, a triangle rising to a still point all of it, two closing
    * alike half each, and with neither approaching (still, or sliding) it sits halfway.
    */
    void testPlanePlacement() {
        const PointOverTriangle setup;
        assert(near(setup.pair.gap, 0.008f));
        assert(near(setup.pair.normal.y, 1.0f));

        assert(near(dividingHeight(setup.pair, moves(Vec3{0.0f, -0.01f, 0.0f}, Vec3{})), 0.0f));
        assert(near(dividingHeight(setup.pair, moves(Vec3{}, Vec3{0.0f, 0.01f, 0.0f})), 0.008f));
        assert(near(dividingHeight(setup.pair, moves(Vec3{0.0f, -0.01f, 0.0f}, Vec3{0.0f, 0.01f, 0.0f})), 0.004f));
        assert(near(dividingHeight(setup.pair, moves(Vec3{}, Vec3{})), 0.004f));
        assert(near(dividingHeight(setup.pair, moves(Vec3{0.01f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -0.01f})), 0.004f));
        assert(near(dividingHeight(setup.pair, moves(Vec3{0.0f, 0.01f, 0.0f}, Vec3{0.0f, -0.01f, 0.0f})), 0.004f));
    }

    // The share of a motion kept (Eq. 16): all of it unless it reaches past 1 / gamma of the way to the plane, else gamma
    // of the way there; motion away from the plane, or along it, is never cut.
    void testCutShare() {
        assert(cutShare(0.01f, 0.0f) == 1.0f);
        assert(cutShare(0.01f, -0.02f) == 1.0f);
        assert(cutShare(0.01f, 0.005f) == 1.0f);
        assert(cutShare(0.01f, 0.01f / (1.0f / clothCutRelaxation) * 1.001f) < 1.0f);
        assert(near(cutShare(0.01f, 0.02f), clothCutRelaxation * 0.5f));
        assert(cutShare(0.0f, 0.01f) == 0.0f);

        // Past the plane by rounding (a gap squeezed to nothing), a point still approaching stops rather than going through.
        assert(cutShare(-1e-9f, 0.01f) == 0.0f);
        assert(cutShare(-1e-9f, -0.01f) == 1.0f);
    }

    /*
    * The cut stops a point short of the plane, never past it; leaves sliding and separating alone; slows anything
    * moving further than half gamma's share of the reach; and never moves a pinned point.
    */
    void testCut() {
        const float wideReach = 0.1f;
        const float reach = 0.01f;
        const std::vector<float> masses{1.0f, 1.0f, 1.0f, 1.0f};

        {
            PointOverTriangle setup;
            std::vector<Vec3> moved = setup.positions;
            moved[0].y = -0.02f;
            cutClothMotion(moved, setup.positions, masses, {setup.pair}, wideReach);
            assert(moved[0].y > 0.0f);
            assert(near(moved[0].y, 0.008f * (1.0f - clothCutRelaxation), 1e-6f));
            assert(moved[1].y == 0.0f && moved[2].y == 0.0f);
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> moved = setup.positions;
            moved[0].x += 0.004f;
            moved[1].y -= 0.003f;
            cutClothMotion(moved, setup.positions, masses, {setup.pair}, reach);
            assert(moved[0].x == setup.positions[0].x + 0.004f);
            assert(moved[1].y == -0.003f);

            std::vector<Vec3> apart = setup.positions;
            apart[0].y += 0.004f;
            cutClothMotion(apart, setup.positions, masses, {setup.pair}, reach);
            assert(apart[0].y == 0.008f + 0.004f);
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> fast = setup.positions;
            fast[2].x += 0.02f;
            cutClothMotion(fast, setup.positions, masses, {}, reach);
            assert(near((fast[2] - setup.positions[2]).length(), 0.5f * clothCutRelaxation * reach, 1e-6f));
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> moved = setup.positions;
            moved[1].y = 0.03f;
            cutClothMotion(moved, setup.positions, {1.0f, 0.0f, 1.0f, 1.0f}, {setup.pair}, wideReach);
            assert(moved[1].y == 0.03f);
        }
    }

    // A point closer than the thickness to a triangle is pushed out to the thickness, shared by mass and weight so no
    // momentum is made; an edge against an edge the same.
    void testContactsPushApart() {
        ClothSettings settings;
        settings.thickness = 0.005f;

        ClothParticles particles;
        particles.positions = {Vec3{0.02f, 0.002f, 0.02f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.1f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(4, Vec3{});
        particles.inverseMasses.assign(4, 1.0f);

        solveSelfContacts(particles, {pointTrianglePair(particles.positions, 0, {1, 2, 3})}, settings);
        const TriangleClosest after = closestTriangleFeature(particles.positions[0], particles.positions[1], particles.positions[2],
                                                             particles.positions[3]);
        assert(near((particles.positions[0] - after.point).length(), settings.thickness, 1e-6f));
        Vec3 total;
        for (const Vec3& position : particles.positions) {
            total = total + position;
        }
        assert(near(total.y, 0.002f, 1e-6f));

        ClothParticles edges;
        edges.positions = {Vec3{-0.05f, 0.003f, 0.0f}, Vec3{0.05f, 0.003f, 0.0f}, Vec3{0.0f, 0.0f, -0.05f}, Vec3{0.0f, 0.0f, 0.05f}};
        edges.previousPositions = edges.positions;
        edges.velocities.assign(4, Vec3{});
        edges.inverseMasses.assign(4, 1.0f);
        const ClothSelfPair crossing = edgeEdgePair(edges.positions, {0, 1}, {2, 3});
        assert(crossing.kind == ClothPairKind::EdgeEdge);
        assert(near(crossing.gap, 0.003f));

        solveSelfContacts(edges, {crossing}, settings);
        assert(near(edges.positions[0].y - edges.positions[2].y, settings.thickness, 1e-6f));
        assert(near(edges.positions[0].y + edges.positions[1].y + edges.positions[2].y + edges.positions[3].y, 0.006f, 1e-6f));
    }

    // Friction as against bodies: a slow slide over a held triangle, shorter than static friction times the push, is undone.
    void testContactFriction() {
        ClothSettings settings;
        ClothParticles particles;
        particles.positions = {Vec3{0.02f, 0.002f, 0.02f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.1f}};
        particles.previousPositions = particles.positions;
        particles.previousPositions[0].x -= 0.0005f;
        particles.velocities.assign(4, Vec3{});
        particles.inverseMasses = {1.0f, 0.0f, 0.0f, 0.0f};

        solveSelfContacts(particles, {pointTrianglePair(particles.positions, 0, {1, 2, 3})}, settings);
        assert(near(particles.positions[0].x, particles.previousPositions[0].x, 1e-6f));
        assert(near(particles.positions[0].y, settings.thickness, 1e-6f));
    }

    // The test oracle: an edge through a triangle it shares no point with is a crossing.
    void testCrossingsCounted() {
        std::vector<std::array<std::uint32_t, 3>> triangles{{0, 1, 2}, {3, 4, 5}};
        const ClothSelfTopology topology = clothSelfTopology(std::vector<Vec3>(6), triangles, {});
        std::vector<Vec3> positions{Vec3{0.0f, 0.0f, 0.0f},   Vec3{1.0f, 0.0f, 0.0f},   Vec3{0.0f, 0.0f, 1.0f},
                                    Vec3{0.2f, -0.5f, 0.2f},  Vec3{0.2f, 0.5f, 0.2f},   Vec3{0.3f, 0.5f, 0.3f}};
        assert(countClothCrossings(positions, triangles, topology) > 0);

        for (std::size_t index = 3; index < 6; ++index) {
            positions[index].y += 1.0f;
        }
        assert(countClothCrossings(positions, triangles, topology) == 0);
    }

    // The upper of two sheets in one cloth, dropped 3 cm onto the lower, held at its corners.
    struct StackedDrop {
        static constexpr int squares = 8;
        Cloth cloth;

        explicit StackedDrop(bool selfCollision) {
            ClothSettings settings;
            settings.selfCollision = selfCollision;
            cloth = makeCloth(stackedSheets(squares, 0.4f, 0.03f), Mat4::identity(), settings);
            const std::uint32_t side = squares + 1;
            addClothPins(cloth, {0, squares, side * squares, side * side - 1});
        }

        float centreHeight(int layer) const {
            const std::uint32_t side = squares + 1;
            return cloth.particles.positions[layer * side * side + (squares / 2) * side + squares / 2].y;
        }
    };

    /*
    * The upper sheet lands on the lower and stays on it, never passing through at any tick; without self-collision it
    * falls through (so the test can fail).
    */
    void testStackedSheets() {
        StackedDrop held{true};
        for (int tick = 0; tick < 120; ++tick) {
            stepCloth(held.cloth, 1.0f / 120.0f);
            assert(countClothCrossings(held.cloth.particles.positions, held.cloth.triangles, held.cloth.selfTopology) == 0);
        }
        assert(held.centreHeight(1) > held.centreHeight(0) + 0.5f * held.cloth.settings.thickness);

        StackedDrop through{false};
        for (int tick = 0; tick < 120; ++tick) {
            stepCloth(through.cloth, 1.0f / 120.0f);
        }
        assert(through.centreHeight(1) < through.centreHeight(0));
    }

    // A long strip dropped on end onto the floor buckles and piles onto itself, and never passes through itself.
    void testStripPilesWithoutCrossing() {
        ClothSettings settings;
        settings.selfCollision = true;
        // Standing on end, its bottom edge 1 cm over the floor (the grid is centred on its middle).
        Cloth strip = makeCloth(Mesh::grid(2, 16, 0.1f, 0.8f), Mat4::translation(0.0f, 0.41f, 0.0f), settings);
        const std::vector<Shape> floor{Aabb{Vec3{-1.0f, -0.1f, -1.0f}, Vec3{1.0f, 0.0f, 1.0f}}};

        // A slight lean, so it buckles one way rather than balancing.
        for (std::size_t index = 0; index < strip.particles.size(); ++index) {
            strip.particles.velocities[index] = Vec3{0.0f, 0.0f, 0.2f * strip.particles.positions[index].y};
        }

        for (int tick = 0; tick < 120; ++tick) {
            stepCloth(strip, 1.0f / 120.0f, floor);
            assert(countClothCrossings(strip.particles.positions, strip.triangles, strip.selfTopology) == 0);
        }

        float highest = 0.0f;
        for (const Vec3& position : strip.particles.positions) {
            highest = std::max(highest, position.y);
        }
        assert(highest < 0.4f);
    }

    // Self-collision keeps the simulation deterministic: two runs agree to the last bit.
    void testDeterministic() {
        StackedDrop first{true};
        StackedDrop second{true};
        for (int tick = 0; tick < 30; ++tick) {
            stepCloth(first.cloth, 1.0f / 120.0f);
            stepCloth(second.cloth, 1.0f / 120.0f);
        }
        for (std::size_t index = 0; index < first.cloth.particles.size(); ++index) {
            const Vec3& a = first.cloth.particles.positions[index];
            const Vec3& b = second.cloth.particles.positions[index];
            assert(a.x == b.x && a.y == b.y && a.z == b.z);
        }
    }

    // Its settings are fields of the cloth's settings, so F5, settings.cfg and the command line all reach them.
    void testSettingsAreReachable() {
        ClothSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };

        assert(!settings.selfCollision);
        assert(setSetting(walk, "selfCollision", "true").outcome == SettingOutcome::Set);
        assert(settings.selfCollision);
        assert(setSetting(walk, "selfCollisionInterval", "3").outcome == SettingOutcome::Set);
        assert(settings.selfCollisionInterval == 3);
        assert(setSetting(walk, "selfCollisionReach", "0.02").outcome == SettingOutcome::Set);
        assert(settings.selfCollisionReach == 0.02f);
        assert(findLimitProblems(walk).empty());
    }
}

void testClothSelfCollision() {
    testNeighbourhood();
    testFlatSheetsAreLeftAlone();
    testPairsAreFound();
    testPlanePlacement();
    testCutShare();
    testCut();
    testContactsPushApart();
    testContactFriction();
    testCrossingsCounted();
    testStackedSheets();
    testStripPilesWithoutCrossing();
    testDeterministic();
    testSettingsAreReachable();
}
