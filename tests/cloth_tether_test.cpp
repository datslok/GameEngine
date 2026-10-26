#include "cloth/cloth.h"
#include "cloth/cloth_tethers.h"
#include "cloth/cloth_yardstick.h"
#include "core/job_system.h"
#include "scene/mesh.h"
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

    // For now the distance along the cloth is the straight line between the points at rest.
    void testRestDistanceIsTheStraightLine() {
        const std::vector<Vec3> rest{Vec3{0.0f, 0.0f, 0.0f}, Vec3{3.0f, 4.0f, 0.0f}};
        assert(near(clothRestDistance(rest, 0, 1), 5.0f, 1e-6f));
        assert(near(clothRestDistance(rest, 1, 0), 5.0f, 1e-6f));
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
        Cloth cloth = yardstickSheet(30, ClothSettings{});
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
            ClothSettings settings;
            settings.tethers = true;
            Cloth tethered = yardstickSheet(squares, settings);
            const float motion = settleYardstick(tethered, 4.0f, tick);
            const SheetMeasure held = measureSheet(tethered, squares);
            assert(held.pathStretch < 0.005f);
            assert(held.sag > 0.99f && held.sag < 1.005f);
            assert(motion < 0.01f);

            Cloth loose = yardstickSheet(squares, ClothSettings{});
            settleYardstick(loose, 4.0f, tick);
            assert(held.pathStretch < measureSheet(loose, squares).pathStretch);
        }
    }

    // Off, the tethers do nothing: the same sheet ends exactly as with none built.
    void testTethersOffDoNothing() {
        ClothSettings off;
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
    testNoAnchorsNoTethers();
    testEachPointTetheredToItsAnchors();
    testNearestAnchorsOnly();
    testTetherPullsBackOntoItsSphere();
    testSameOnAnyThreads();
    testPinningRebuildsTethers();
    testTethersHoldTheYardstick();
    testTethersOffDoNothing();
}
