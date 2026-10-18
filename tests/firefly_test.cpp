#include "gameplay/firefly.h"
#include "math/transform.h"
#include "scene/interpolation.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float a, float b) {
        return std::abs(a - b) < 0.001f;
    }

    Firefly sampleFirefly() {
        return Firefly{
            .centre = Vec3{3.0f, 0.0f, -2.0f},
            .orbitRadius = 0.75f,
            .radiusWobble = 0.2f,
            .height = 1.0f,
            .heightWobble = 0.35f,
            .orbitSpeed = 0.6f,
            .phase = 1.0f
        };
    }

    // However long it flies, it stays in a ring around its centre: never closer than the radius minus the wobble (so it
    // cannot fly into the pillar it circles), never further than the radius plus it, and between its lowest and highest heights.
    void testStaysInItsRing() {
        const Firefly firefly = sampleFirefly();

        for (int step = 0; step < 10000; ++step) {
            const double seconds = step * 0.37;
            const Vec3 position = fireflyPosition(firefly, seconds);
            const Vec3 fromCentre = position - firefly.centre;
            const float acrossGround = Vec3{fromCentre.x, 0.0f, fromCentre.z}.length();

            assert(acrossGround >= firefly.orbitRadius - firefly.radiusWobble - 0.001f);
            assert(acrossGround <= firefly.orbitRadius + firefly.radiusWobble + 0.001f);
            assert(fromCentre.y >= firefly.height - firefly.heightWobble - 0.001f);
            assert(fromCentre.y <= firefly.height + firefly.heightWobble + 0.001f);
        }
    }

    // It goes all the way round its centre, so the shadow it throws sweeps every direction.
    void testCirclesItsCentre() {
        const Firefly firefly = sampleFirefly();
        bool seen[4] = {false, false, false, false};

        for (int step = 0; step < 2000; ++step) {
            const Vec3 fromCentre = fireflyPosition(firefly, step * 0.05) - firefly.centre;
            const int quadrant = (fromCentre.x >= 0.0f ? 0 : 1) + (fromCentre.z >= 0.0f ? 0 : 2);
            seen[quadrant] = true;
        }

        assert(seen[0] && seen[1] && seen[2] && seen[3]);
    }

    // Moving smoothly: a tick later it has moved, but only a little (no jumps for interpolation to smear).
    void testMovesSmoothly() {
        const Firefly firefly = sampleFirefly();
        constexpr double tick = 1.0 / 120.0;

        for (int step = 0; step < 1000; ++step) {
            const double seconds = step * 0.71;
            const float moved = (fireflyPosition(firefly, seconds + tick) - fireflyPosition(firefly, seconds)).length();
            assert(moved < 0.1f);
        }

        assert((fireflyPosition(firefly, 1.0) - fireflyPosition(firefly, 0.0)).length() > 0.1f);
    }

    // Two fireflies with different phases are in different places at the same moment, so a swarm does not move in step.
    void testPhaseDesynchronises() {
        Firefly first = sampleFirefly();
        Firefly second = sampleFirefly();
        second.phase = 2.5f;

        assert((fireflyPosition(first, 10.0) - fireflyPosition(second, 10.0)).length() > 0.1f);
    }

    // The system moves the Transform to the position for the given time and leaves its rotation and scale alone. The
    // engine saves PreviousTransform before each tick, so the drawn light glides between ticks.
    void testUpdateMovesTransform() {
        World world;
        const Firefly firefly = sampleFirefly();

        Transform start;
        start.position = fireflyPosition(firefly, 0.0);
        start.scale = Vec3{0.12f, 0.12f, 0.12f};
        start.rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.5f);

        const Entity entity = world.create();
        world.add(entity, start);
        world.add(entity, PreviousTransform{start});
        world.add(entity, firefly);

        updateFireflies(world, 2.0);

        const Transform& moved = world.get<Transform>(entity);
        const Vec3 expected = fireflyPosition(firefly, 2.0);
        assert(nearlyEqual(moved.position.x, expected.x));
        assert(nearlyEqual(moved.position.y, expected.y));
        assert(nearlyEqual(moved.position.z, expected.z));
        assert(nearlyEqual(moved.scale.x, 0.12f));
        assert(angleBetween(moved.rotation, start.rotation) < 0.0001f);
    }
}

void testFirefly() {
    testStaysInItsRing();
    testCirclesItsCentre();
    testMovesSmoothly();
    testPhaseDesynchronises();
    testUpdateMovesTransform();
}
