#include "cloth/cloth.h"
#include "cloth/cloth_tethers.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
#include "cloth_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    // A 2 x 2 m sheet of 4 x 4 squares standing in the plane z = 0, its middle at the origin: point (row, column) is
    // row * 5 + column, rows counted up from the hem at y = -1, columns from x = -1, half a metre apart.
    Cloth standingSheet() {
        return makeCloth(Mesh::grid(4, 4, 2.0f, 2.0f), Mat4::identity(), ClothSettings{});
    }

    // The tether in slot rank of a point (0 the nearest anchor).
    const ClothTether& tetherOf(const std::vector<ClothTether>& tethers, std::size_t point, std::size_t rank) {
        return tethers[point * tethersPerPoint + rank];
    }

    // The distance from a point to each anchor along the cloth at rest.
    std::vector<float> restDistances(const ClothRestPaths& paths, std::uint32_t point) {
        std::vector<float> distances;
        paths.distances(point, distances);
        return distances;
    }

    // A cloth that is not cut into pieces is measured by straight lines between its points at rest.
    void testRestDistanceIsTheStraightLine() {
        const std::vector<Vec3> rest{Vec3{0.0f, 0.0f, 0.0f}, Vec3{3.0f, 4.0f, 0.0f}};
        assert(near(restDistances(ClothRestPaths{rest, ClothPieces{}, {1}}, 0)[0], 5.0f, 1e-6f));
        assert(near(restDistances(ClothRestPaths{rest, ClothPieces{}, {0}}, 1)[0], 5.0f, 1e-6f));
    }

    /*
    * An L-shaped piece, 2 m along each leg, its inward corner at (1, 1), with one more point inside its foot: along the
    * fabric, the top of the upright to the end of the foot goes round the corner (1 + 1, not the straight 1.41 across
    * the gap), a line that only grazes the corner is straight, and a point that sees the anchor straight is too. A
    * point on another piece is no distance at all from it (infinity: no tether crosses a seam).
    */
    void testRestDistanceRoundAnInwardCorner() {
        const std::vector<Vec3> rest{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 0.0f}, Vec3{1.0f, 1.0f, 0.0f},
                                     Vec3{1.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, Vec3{1.5f, 0.5f, 0.0f}, Vec3{5.0f, 5.0f, 0.0f}};
        ClothPieces pieces;
        pieces.pieceOf = {0, 0, 0, 0, 0, 0, 0, 1};
        pieces.outlines = {{0, 1, 2, 3, 4, 5}, {7}};

        const ClothRestPaths paths{rest, pieces, {4, 1}};
        const std::vector<float> fromFootEnd = restDistances(paths, 2);
        assert(near(fromFootEnd[0], 2.0f, 1e-5f) && near(fromFootEnd[1], 1.0f, 1e-5f));
        assert(near(restDistances(paths, 5)[1], 2.8284271f, 1e-5f));
        const std::vector<float> fromInside = restDistances(paths, 6);
        assert(near(fromInside[0], 0.7071068f + 1.0f, 1e-5f) && near(fromInside[1], 0.7071068f, 1e-5f));
        assert(std::isinf(restDistances(paths, 7)[0]));
    }

    /*
    * A cloth in two pieces, each pinned at one point: every point is tethered to its own piece's anchor alone, at its
    * distance round the piece's inward corner.
    */
    void testTethersStayOnTheirPiece() {
        const std::vector<Vec3> rest{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 0.0f}, Vec3{1.0f, 1.0f, 0.0f},
                                     Vec3{1.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f},
                                     Vec3{3.0f, 0.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f}, Vec3{3.0f, 1.0f, 0.0f}};
        ClothPieces pieces;
        pieces.pieceOf = {0, 0, 0, 0, 0, 0, 1, 1, 1};
        pieces.outlines = {{0, 1, 2, 3, 4, 5}, {6, 7, 8}};
        std::vector<float> inverseMasses(rest.size(), 1.0f);
        inverseMasses[4] = 0.0f;
        inverseMasses[6] = 0.0f;

        const std::vector<ClothTether> tethers = clothTethers(rest, inverseMasses, pieces);
        assert(tetherOf(tethers, 2, 0).anchor == 4 && near(tetherOf(tethers, 2, 0).restLength, 2.0f, 1e-5f));
        assert(tetherOf(tethers, 2, 1).anchor == noTetherAnchor);
        assert(tetherOf(tethers, 7, 0).anchor == 6 && near(tetherOf(tethers, 7, 0).restLength, 1.0f, 1e-5f));
        assert(tetherOf(tethers, 7, 1).anchor == noTetherAnchor);
    }

    // Pinning several points at once is pinning them one after another, with the levels and tethers rebuilt once.
    void testPinningSeveralAtOnce() {
        Cloth one = standingSheet();
        addClothPin(one, 20);
        addClothPin(one, 24);
        Cloth both = standingSheet();
        addClothPins(both, {20, 24});

        assert(both.pins.size() == 2 && both.pins[0].particle == 20 && both.pins[1].particle == 24);
        assert(both.particles.inverseMasses == one.particles.inverseMasses);
        assert(both.tethers.size() == one.tethers.size());
        for (std::size_t index = 0; index < both.tethers.size(); ++index) {
            assert(both.tethers[index].anchor == one.tethers[index].anchor && both.tethers[index].restLength == one.tethers[index].restLength);
        }
        assert(both.hierarchy.levels.size() == one.hierarchy.levels.size());
    }

    // A cloth with no pinned point has nothing to hang from, so no tethers at all.
    void testNoAnchorsNoTethers() {
        const Cloth cloth = standingSheet();
        assert(clothTethers(cloth.restPositions, cloth.particles.inverseMasses).empty());
        assert(cloth.tethers.empty());
    }

    /*
    * Pinned at its top corners, every free point is tethered to both, nearest first (a tie in the order of the anchors),
    * at its straight distance at rest; the spare slots and the pinned points' slots hold no anchor.
    */
    void testEachPointTetheredToItsAnchors() {
        Cloth cloth = standingSheet();
        pinParticle(cloth.particles, 20);
        pinParticle(cloth.particles, 24);
        const std::vector<ClothTether> tethers = clothTethers(cloth.restPositions, cloth.particles.inverseMasses);
        assert(tethers.size() == cloth.particles.size() * tethersPerPoint);

        // The hem's middle, (0, -1): as far from each corner, sqrt(5).
        assert(tetherOf(tethers, 2, 0).anchor == 20 && near(tetherOf(tethers, 2, 0).restLength, 2.2360680f, 1e-6f));
        assert(tetherOf(tethers, 2, 1).anchor == 24 && near(tetherOf(tethers, 2, 1).restLength, 2.2360680f, 1e-6f));
        assert(tetherOf(tethers, 2, 2).anchor == noTetherAnchor && tetherOf(tethers, 2, 3).anchor == noTetherAnchor);

        // Beside the left corner on the top edge: the left corner first.
        assert(tetherOf(tethers, 21, 0).anchor == 20 && near(tetherOf(tethers, 21, 0).restLength, 0.5f, 1e-6f));
        assert(tetherOf(tethers, 21, 1).anchor == 24 && near(tetherOf(tethers, 21, 1).restLength, 1.5f, 1e-6f));

        for (const std::size_t pinned : {20u, 24u}) {
            for (std::size_t rank = 0; rank < tethersPerPoint; ++rank) {
                assert(tetherOf(tethers, pinned, rank).anchor == noTetherAnchor);
            }
        }
    }

    // With more anchors than slots each point keeps its nearest few, nearest first, ties in the order of the anchors.
    void testNearestAnchorsOnly() {
        Cloth cloth = standingSheet();
        for (const std::size_t pinned : {0u, 20u, 21u, 22u, 23u, 24u}) {
            pinParticle(cloth.particles, pinned);
        }
        const std::vector<ClothTether> tethers = clothTethers(cloth.restPositions, cloth.particles.inverseMasses);

        // Point 10, (-1, 0): a metre from the bottom and top left corners, then along the top row.
        assert(tetherOf(tethers, 10, 0).anchor == 0 && near(tetherOf(tethers, 10, 0).restLength, 1.0f, 1e-6f));
        assert(tetherOf(tethers, 10, 1).anchor == 20 && near(tetherOf(tethers, 10, 1).restLength, 1.0f, 1e-6f));
        assert(tetherOf(tethers, 10, 2).anchor == 21 && near(tetherOf(tethers, 10, 2).restLength, 1.1180340f, 1e-6f));
        assert(tetherOf(tethers, 10, 3).anchor == 22 && near(tetherOf(tethers, 10, 3).restLength, 1.4142135f, 1e-6f));
    }

    /*
    * A tether is a string with slack: a point further from its anchor than the rest length and the slack goes straight
    * back onto that sphere, a point inside it is left exactly where it is, and the anchor never moves.
    */
    void testTetherPullsBackOntoItsSphere() {
        ClothParticles particles;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities = std::vector<Vec3>(3, Vec3{0.0f, 0.0f, 0.0f});
        particles.inverseMasses = {0.0f, 1.0f, 1.0f};
        const std::vector<ClothTether> tethers = clothTethers(particles.positions, particles.inverseMasses);

        particles.positions[1] = Vec3{0.0f, 3.0f, 0.0f};
        particles.positions[2] = Vec3{0.5f, 0.5f, 0.0f};
        solveTethers(particles, tethers, 0.1f, serialJobs());
        assert(near(particles.positions[1].x, 0.0f, 1e-6f) && near(particles.positions[1].y, 1.1f, 1e-6f) && near(particles.positions[1].z, 0.0f, 1e-6f));
        assert(particles.positions[2].x == 0.5f && particles.positions[2].y == 0.5f && particles.positions[2].z == 0.0f);
        assert(particles.positions[0].x == 0.0f && particles.positions[0].y == 0.0f && particles.positions[0].z == 0.0f);

        // No slack: back to the rest length itself.
        particles.positions[1] = Vec3{2.0f, 0.0f, 0.0f};
        solveTethers(particles, tethers, 0.0f, serialJobs());
        assert(near(particles.positions[1].x, 1.0f, 1e-6f));
    }

    // Each point's tethers touch only that point, so the points can be shared among threads with the same result to the last bit.
    void testSameOnAnyThreads() {
        Cloth cloth = yardstickSheet(30, withoutSelfCollision());
        const Vec3 pin = cloth.particles.positions[cloth.pins[0].particle];
        for (std::size_t index = 0; index < cloth.particles.size(); ++index) {
            Vec3& position = cloth.particles.positions[index];
            position = pin + (position - pin) * 1.3f + Vec3{0.0f, 0.0f, 0.01f * static_cast<float>(index % 7)};
        }
        ClothParticles serial = cloth.particles;
        ClothParticles shared = cloth.particles;

        JobSystem jobs;
        solveTethers(serial, cloth.tethers, 0.002f, serialJobs());
        solveTethers(shared, cloth.tethers, 0.002f, jobs);
        for (std::size_t index = 0; index < serial.size(); ++index) {
            assert(serial.positions[index].x == shared.positions[index].x);
            assert(serial.positions[index].y == shared.positions[index].y);
            assert(serial.positions[index].z == shared.positions[index].z);
        }
    }

    // Pinning a point, either way, rebuilds the tethers so it anchors the cloth.
    void testPinningRebuildsTethers() {
        Cloth cloth = standingSheet();
        addClothPin(cloth, 20);
        assert(cloth.tethers.size() == cloth.particles.size() * tethersPerPoint);
        assert(tetherOf(cloth.tethers, 2, 0).anchor == 20 && tetherOf(cloth.tethers, 2, 1).anchor == noTetherAnchor);

        pinClothPoint(cloth, 24);
        assert(tetherOf(cloth.tethers, 2, 1).anchor == 24);
    }

    /*
    * What tethers are for: the yardstick's sheet, which XPBD alone lets stretch more on finer meshes (1% at 25 squares,
    * 3.5% at 50), hangs within half a percent at both, settles, and stretches less than without them.
    */
    void testTethersHoldTheYardstick() {
        for (const int squares : {25, 50}) {
            ClothSettings settings = withoutSelfCollision();
            settings.tethers = true;
            Cloth tethered = yardstickSheet(squares, settings);
            const float motion = settleYardstick(tethered, 4.0f, tick);
            const SheetMeasure held = measureSheet(tethered, squares);
            assert(held.pathStretch < 0.005f);
            assert(held.sag > 0.99f && held.sag < 1.005f);
            assert(motion < 0.01f);

            ClothSettings untethered = withoutSelfCollision();
            untethered.tethers = false;
            Cloth loose = yardstickSheet(squares, untethered);
            settleYardstick(loose, 4.0f, tick);
            assert(held.pathStretch < measureSheet(loose, squares).pathStretch);
        }
    }

    // On by default, since the yardstick judged them: every pinned cloth hangs at its fabric's stretch at any resolution.
    void testTethersOnByDefault() {
        assert(ClothSettings{}.tethers);
        assert(ClothSettings{}.tetherSlack == 0.002f);
    }

    // Off, the tethers do nothing: the same sheet ends exactly as with none built.
    void testTethersOffDoNothing() {
        ClothSettings off = withoutSelfCollision();
        off.tethers = false;
        Cloth withTethers = yardstickSheet(10, off);
        Cloth without = withTethers;
        without.tethers.clear();
        settleYardstick(withTethers, 1.0f, tick);
        settleYardstick(without, 1.0f, tick);
        for (std::size_t index = 0; index < without.particles.size(); ++index) {
            assert(withTethers.particles.positions[index].x == without.particles.positions[index].x);
            assert(withTethers.particles.positions[index].y == without.particles.positions[index].y);
            assert(withTethers.particles.positions[index].z == without.particles.positions[index].z);
        }
    }
}

void testClothTethers() {
    testRestDistanceIsTheStraightLine();
    testRestDistanceRoundAnInwardCorner();
    testTethersStayOnTheirPiece();
    testPinningSeveralAtOnce();
    testNoAnchorsNoTethers();
    testEachPointTetheredToItsAnchors();
    testNearestAnchorsOnly();
    testTetherPullsBackOntoItsSphere();
    testSameOnAnyThreads();
    testPinningRebuildsTethers();
    testTethersHoldTheYardstick();
    testTethersOnByDefault();
    testTethersOffDoNothing();
}
