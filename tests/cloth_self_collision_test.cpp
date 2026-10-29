#include "cloth/cloth.h"
#include "cloth/cloth_self_collision.h"
#include "engine/settings_file.h"
#include "math/closest_points.h"

#include "cloth_helpers.h"

#include <algorithm>
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
    * Neighbours are excused from meeting when they started closer than the thickness, or lie on different pieces (across
    * a seam, where the flat pattern says nothing of how close they will be sewn); features further apart never are.
    */
    void testNeighboursAcrossSeamsAreExcused() {
        const FlatGrid grid{2, 1.0f}; // points row * 3 + column
        const ClothSelfTopology onePiece = clothSelfTopology(grid.positions, grid.triangles, {});
        const std::array<std::uint32_t, 1> corner{0};
        const std::array<std::uint32_t, 3> neighbouring{1, 4, 2};
        const std::array<std::uint32_t, 3> far{5, 8, 7};
        const float thickness = 0.005f;

        assert(!clothPairExcused(onePiece, corner, neighbouring, 0.4f, thickness));
        assert(clothPairExcused(onePiece, corner, neighbouring, 0.004f, thickness));
        assert(!clothPairExcused(onePiece, corner, far, 0.004f, thickness));

        // The right column (2, 5, 8) a piece of its own: point 1 beside it is a neighbour across the seam.
        const ClothSelfTopology twoPieces = clothSelfTopology(grid.positions, grid.triangles, {}, {0, 0, 1, 0, 0, 1, 0, 0, 1});
        const std::array<std::uint32_t, 1> beside{1};
        const std::array<std::uint32_t, 2> seam{2, 5};
        assert(clothPairExcused(twoPieces, beside, seam, 0.4f, thickness));
        assert(!clothPairExcused(twoPieces, corner, far, 0.4f, thickness));
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

    // A cut by freshly found pairs (their planes not yet placed): the pairs are taken by value, as each test makes them.
    void cutFresh(std::vector<Vec3>& positions, const std::vector<Vec3>& start, const std::vector<Vec3>& substepStart, const std::vector<float>& masses,
                  std::vector<ClothSelfPair> pairs, float reach) {
        cutClothMotion(positions, start, substepStart, masses, pairs, reach);
    }

    /*
    * A pair's plane is placed at its first cut and kept until the pairs are found again: a later substep's different
    * speeds of approach do not move it.
    */
    void testPlanesAreKept() {
        PointOverTriangle setup;
        std::vector<ClothSelfPair> pairs{setup.pair};
        assert(pairs[0].plane < 0.0f);
        const std::vector<float> masses{1.0f, 1.0f, 1.0f, 1.0f};

        // The point falls: the plane is placed on the triangle, all the room the point's.
        std::vector<Vec3> falling = setup.positions;
        falling[0].y -= 0.001f;
        cutClothMotion(falling, setup.positions, setup.positions, masses, pairs, 0.1f);
        assert(near(pairs[0].plane, 0.0f));

        // Then the triangle rises instead: placed afresh the plane would go to the point, but it stays.
        std::vector<Vec3> rising = falling;
        for (std::size_t corner = 1; corner < 4; ++corner) {
            rising[corner].y += 0.002f;
        }
        cutClothMotion(rising, setup.positions, falling, masses, pairs, 0.1f);
        assert(near(pairs[0].plane, 0.0f));
        assert(rising[1].y < 0.002f);
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
            cutFresh(moved, setup.positions, setup.positions, masses, {setup.pair}, wideReach);
            assert(moved[0].y > 0.0f);
            assert(near(moved[0].y, 0.008f * (1.0f - clothCutRelaxation), 1e-6f));
            assert(moved[1].y == 0.0f && moved[2].y == 0.0f);
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> moved = setup.positions;
            moved[0].x += 0.004f;
            moved[1].y -= 0.003f;
            cutFresh(moved, setup.positions, setup.positions, masses, {setup.pair}, reach);
            assert(moved[0].x == setup.positions[0].x + 0.004f);
            assert(moved[1].y == -0.003f);

            std::vector<Vec3> apart = setup.positions;
            apart[0].y += 0.004f;
            cutFresh(apart, setup.positions, setup.positions, masses, {setup.pair}, reach);
            assert(apart[0].y == 0.008f + 0.004f);
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> fast = setup.positions;
            fast[2].x += 0.02f;
            cutFresh(fast, setup.positions, setup.positions, masses, {}, reach);
            assert(near((fast[2] - setup.positions[2]).length(), 0.5f * clothCutRelaxation * reach, 1e-6f));
        }
        {
            /*
            * Only this substep's motion is cut, from where the last substep left the point (3 mm lower than when the pair
            * was found): it stops gamma of the way from there to the plane. The cap counts what it already moved.
            */
            PointOverTriangle setup;
            std::vector<Vec3> substepStart = setup.positions;
            substepStart[0].y = 0.005f;
            std::vector<Vec3> moved = substepStart;
            moved[0].y = -0.02f;
            cutFresh(moved, setup.positions, substepStart, masses, {setup.pair}, wideReach);
            assert(near(moved[0].y, 0.005f * (1.0f - clothCutRelaxation), 1e-6f));

            std::vector<Vec3> sliding = substepStart;
            sliding[0].x += 0.004f;
            cutFresh(sliding, setup.positions, substepStart, masses, {}, reach);
            assert(near(sliding[0].x - substepStart[0].x, 0.5f * clothCutRelaxation * reach - 0.003f, 1e-6f));
        }
        {
            PointOverTriangle setup;
            std::vector<Vec3> moved = setup.positions;
            moved[1].y = 0.03f;
            cutFresh(moved, setup.positions, setup.positions, {1.0f, 0.0f, 1.0f, 1.0f}, {setup.pair}, wideReach);
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

    // Moves are summed in whole nanometres, so any order adds up to the same: rounded half to even, clamped to the range.
    void testFixedPointMoves() {
        assert(selfMoveUnits(0.0f) == 0);
        assert(selfMoveUnits(0.001f) == 1000000);
        assert(selfMoveUnits(-0.0025f) == -2500000);
        assert(selfMoveUnits(1.5e-9f) == 2);
        assert(selfMoveUnits(2.5e-9f) == 2);
        assert(selfMoveUnits(100.0f) == selfMoveUnits(clothSelfLargestMove));
        assert(selfMoveUnits(-100.0f) == -selfMoveUnits(clothSelfLargestMove));
        assert(std::fabs(selfMoveMetres(1000000, 1) - 0.001f) < 1e-9f);
        assert(std::fabs(selfMoveMetres(3000000, 2) - 0.0015f) < 1e-9f);
    }

    /*
    * The contacts are solved all at once (Jacobi), as the GPU solves them: each pair's push worked out from the points as
    * they were, each point moved by the average of the pushes it got. Two triangles under one point, each 2 mm below it
    * and each wanting it 3 mm higher, lift it as one would, not twice as far.
    */
    void testSharedPointsTakeTheAverage() {
        ClothSettings settings;
        settings.staticFriction = 0.0f;
        settings.kineticFriction = 0.0f;
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.002f, 0.0f},   Vec3{-0.1f, 0.0f, -0.1f}, Vec3{0.1f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.1f},
                               Vec3{-0.1f, 0.0f, 0.1f}, Vec3{0.1f, 0.0f, 0.1f},  Vec3{0.0f, 0.0f, -0.12f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(particles.positions.size(), Vec3{});
        particles.inverseMasses = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

        const std::vector<ClothSelfPair> pairs{pointTrianglePair(particles.positions, 0, {1, 2, 3}), pointTrianglePair(particles.positions, 0, {4, 5, 6})};
        solveSelfContacts(particles, pairs, settings);
        assert(near(particles.positions[0].y, settings.thickness, 1e-8f));
        assert(near(particles.positions[0].x, 0.0f, 1e-8f));
    }

    /*
    * The pairs' order makes no difference, to the last bit: the GPU finds them in whatever order its invocations run,
    * so a pile of layers whose pairs share points ends exactly the same however its list is shuffled.
    */
    void testContactOrderMakesNoDifference() {
        ClothSettings settings;
        const Cloth cloth = makeCloth(stackedSheets(6, 0.3f, 0.003f), Mat4::identity(), settings);
        std::vector<ClothSelfPair> pairs = findClothPairs(cloth.particles.positions, cloth.triangles, cloth.selfTopology, 0.0075f, settings.thickness);
        assert(pairs.size() > 20);

        ClothParticles inOrder = cloth.particles;
        solveSelfContacts(inOrder, pairs, settings);

        std::vector<ClothSelfPair> shuffled = pairs;
        std::reverse(shuffled.begin(), shuffled.end());
        std::rotate(shuffled.begin(), shuffled.begin() + static_cast<std::ptrdiff_t>(shuffled.size() / 3), shuffled.end());
        ClothParticles reordered = cloth.particles;
        solveSelfContacts(reordered, shuffled, settings);

        for (std::size_t index = 0; index < inOrder.size(); ++index) {
            const Vec3& a = inOrder.positions[index];
            const Vec3& b = reordered.positions[index];
            assert(a.x == b.x && a.y == b.y && a.z == b.z);
        }

        // And the layers were pushed apart.
        float lowest = 1.0f;
        float highest = -1.0f;
        const std::size_t layer = inOrder.size() / 2;
        for (std::size_t index = 0; index < layer; ++index) {
            highest = std::max(highest, inOrder.positions[index].y);
            lowest = std::min(lowest, inOrder.positions[layer + index].y);
        }
        assert(lowest - highest > 0.003f);
    }

    /*
    * A stitch's two points move as one: a shut stitch (both points on one spot, of different masses) over a held
    * triangle, only the first in a pair, is pushed off as one point; and the cut gives both the smaller share.
    */
    void testStitchedPointsMoveTogether() {
        ClothSettings settings;
        settings.staticFriction = 0.0f;
        settings.kineticFriction = 0.0f;
        ClothParticles particles;
        particles.positions = {Vec3{0.02f, 0.002f, 0.02f}, Vec3{0.02f, 0.002f, 0.02f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f},
                               Vec3{0.0f, 0.0f, 0.1f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(5, Vec3{});
        particles.inverseMasses = {1.0f, 3.0f, 0.0f, 0.0f, 0.0f};
        const std::vector<std::uint32_t> partners{1, 0, clothNoPartner, clothNoPartner, clothNoPartner};

        solveSelfContacts(particles, {pointTrianglePair(particles.positions, 0, {2, 3, 4})}, settings, {}, partners);
        assert(particles.positions[0].y == particles.positions[1].y && particles.positions[0].x == particles.positions[1].x);
        assert(near(particles.positions[0].y, settings.thickness, 1e-6f));

        // The first's pair lets it fall a little; the second has none, but takes the first's share.
        const std::vector<Vec3> start = particles.positions;
        std::vector<Vec3> moved = start;
        moved[0].y -= 0.01f;
        moved[1].y -= 0.01f;
        std::vector<ClothSelfPair> pairs{pointTrianglePair(start, 0, {2, 3, 4})};
        cutClothMotion(moved, start, start, particles.inverseMasses, pairs, 0.1f, partners);
        assert(moved[0].y > 0.0f && moved[0].y == moved[1].y);
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

        // On by default since it runs on the GPU too.
        assert(settings.selfCollision);
        assert(setSetting(walk, "selfCollision", "false").outcome == SettingOutcome::Set);
        assert(!settings.selfCollision);
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
    testNeighboursAcrossSeamsAreExcused();
    testPairsAreFound();
    testPlanePlacement();
    testCutShare();
    testCut();
    testPlanesAreKept();
    testContactsPushApart();
    testContactFriction();
    testStitchedPointsMoveTogether();
    testFixedPointMoves();
    testSharedPointsTakeTheAverage();
    testContactOrderMakesNoDifference();
    testCrossingsCounted();
    testStackedSheets();
    testStripPilesWithoutCrossing();
    testDeterministic();
    testSettingsAreReachable();
}
