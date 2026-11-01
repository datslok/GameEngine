#include "cloth/cloth.h"
#include "cloth/cloth_grab.h"
#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearVec(const Vec3& actual, const Vec3& expected, float tolerance) {
        return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) && near(actual.z, expected.z, tolerance);
    }

    // A 2 x 2 m sheet of 4 x 4 squares standing in the plane z = 0, facing +z, its middle at the origin.
    Cloth standingSheet() {
        return makeCloth(Mesh::grid(4, 4, 2.0f, 2.0f), Mat4::identity(), ClothSettings{});
    }

    /*
    * A ray meets the sheet as it is now and gives the corner of the hit triangle nearest the hit, the hit itself and its
    * distance; out of reach, or pointing away, it gives nothing.
    */
    void testRaysFindTheNearestCorner() {
        Cloth cloth = standingSheet();
        const std::optional<ClothHit> hit = raycastCloth(cloth, Ray{Vec3{0.45f, 0.05f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}}, 10.0f);
        assert(hit.has_value());
        assert(near(hit->distance, 3.0f, 1e-5f));
        assert(nearVec(hit->point, Vec3{0.45f, 0.05f, 0.0f}, 1e-5f));
        assert(nearVec(cloth.particles.positions[hit->particle], Vec3{0.5f, 0.0f, 0.0f}, 1e-6f));

        assert(!raycastCloth(cloth, Ray{Vec3{0.45f, 0.05f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}}, 2.0f).has_value());
        assert(!raycastCloth(cloth, Ray{Vec3{0.45f, 0.05f, 3.0f}, Vec3{0.0f, 0.0f, 1.0f}}, 10.0f).has_value());

        // Moved, the sheet is found where it is now: its middle point pushed back half a metre.
        cloth.particles.positions[12].z = -0.5f;
        const std::optional<ClothHit> moved = raycastCloth(cloth, Ray{Vec3{0.02f, 0.01f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}}, 10.0f);
        assert(moved.has_value());
        assert(moved->particle == 12);
        assert(near(moved->distance, 3.5f, 0.05f) && moved->distance > 3.4f);
    }

    // A hand with no give puts a free point exactly on its target in one solve, and cannot move a pinned one.
    void testGrabPullsToTheTarget() {
        Cloth cloth = standingSheet();
        solveGrab(cloth.particles, ClothGrab{6, Vec3{1.0f, 2.0f, 3.0f}}, 0.0f, 0.001f);
        assert(nearVec(cloth.particles.positions[6], Vec3{1.0f, 2.0f, 3.0f}, 1e-5f));

        pinParticle(cloth.particles, 7);
        const Vec3 pinned = cloth.particles.positions[7];
        solveGrab(cloth.particles, ClothGrab{7, Vec3{1.0f, 2.0f, 3.0f}}, 0.0f, 0.001f);
        assert(nearVec(cloth.particles.positions[7], pinned, 0.0f));
    }

    /*
    * The hand is a stiff spring: a weight of m kg held by it alone hangs below the target by m g alpha. Held at the
    * origin, where floats are finest: at 30 substeps gravity moves the weight 0.76 micrometres a substep, which a float
    * a metre up resolves only to 0.06, and there the stretch settles 1.65% off (measured; see cloth_solver_test).
    */
    void testGrabIsASpring() {
        ClothParticles particles;
        const float mass = 0.2f;
        particles.positions = {Vec3{0.0f, 0.0f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities = {Vec3{0.0f, 0.0f, 0.0f}};
        particles.inverseMasses = {1.0f / mass};

        ClothSettings settings;
        settings.grabCompliance = 0.01f;
        settings.drag = 5.0f;
        const ClothGrab grab{0, Vec3{0.0f, 0.0f, 0.0f}};
        for (int tick = 0; tick < 1200; ++tick) {
            stepClothParticles(particles, {}, settings, 1.0f / 120.0f, grab);
        }
        const float expected = mass * settings.gravity * settings.grabCompliance;
        assert(near(-particles.positions[0].y, expected, 0.01f * expected));
    }

    // Held, a point follows the hand through the steps.
    void testHeldPointsFollow() {
        Cloth cloth = standingSheet();
        pinParticle(cloth.particles, 20);
        pinParticle(cloth.particles, 24);

        // Pulled out of the sheet's plane (the hand gets there at its speed, 0.7 m in about a quarter of a second), then
        // sideways at 1.2 m/s, within the sheet's reach of its pins. It keeps up on average; tick to tick it jiggles a
        // few millimetres on the hand's spring (measured: about 80 Hz), so its speed at any one tick says little.
        cloth.grab = ClothGrab{2, Vec3{0.0f, -0.5f, 0.5f}};
        for (int tick = 0; tick < 40; ++tick) {
            stepCloth(cloth, 1.0f / 120.0f);
        }
        float halfway = 0.0f;
        for (int tick = 0; tick < 60; ++tick) {
            cloth.grab = ClothGrab{2, Vec3{0.01f * tick, -0.5f, 0.5f}};
            stepCloth(cloth, 1.0f / 120.0f);
            if (tick == 29) {
                halfway = cloth.particles.positions[2].x;
            }
        }
        assert(nearVec(cloth.particles.positions[2], Vec3{0.01f * 59, -0.5f, 0.5f}, 0.05f));
        const float averageSpeed = (cloth.particles.positions[2].x - halfway) / (30.0f / 120.0f);
        assert(near(averageSpeed, 1.2f, 0.2f));
    }

    /*
    * The hand's way through a tick (moveClothHand): none without a grab; a new grab starts at the held point, not where
    * the game aims, so taking hold never yanks; each tick it goes towards the target no further than the hand's speed
    * times the tick, and stops on it; holding the same point it carries on from where it got to, and another point
    * starts again where that point is.
    */
    void testTheHandHasASpeedLimit() {
        const std::vector<Vec3> positions{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
        const float tick = 1.0f / 120.0f;
        const float speed = 3.0f;
        assert(!moveClothHand(std::nullopt, std::nullopt, positions, speed, tick));

        const std::optional<ClothHandMove> first = moveClothHand(std::nullopt, ClothGrab{0, Vec3{1.0f, 0.0f, 0.0f}}, positions, speed, tick);
        assert(first && first->particle == 0);
        assert(nearVec(first->start, positions[0], 0.0f));
        assert(nearVec(first->end, Vec3{speed * tick, 0.0f, 0.0f}, 1e-6f));

        const std::optional<ClothHandMove> second = moveClothHand(first, ClothGrab{0, Vec3{1.0f, 0.0f, 0.0f}}, positions, speed, tick);
        assert(nearVec(second->start, first->end, 0.0f));
        assert(nearVec(second->end, Vec3{2.0f * speed * tick, 0.0f, 0.0f}, 1e-6f));

        const ClothHandMove close{0, Vec3{0.99f, 0.0f, 0.0f}, Vec3{0.99f, 0.0f, 0.0f}};
        assert(nearVec(moveClothHand(close, ClothGrab{0, Vec3{1.0f, 0.0f, 0.0f}}, positions, speed, tick)->end, Vec3{1.0f, 0.0f, 0.0f}, 0.0f));

        const std::optional<ClothHandMove> other = moveClothHand(second, ClothGrab{1, Vec3{1.0f, 1.0f, 0.0f}}, positions, speed, tick);
        assert(other->particle == 1 && nearVec(other->start, positions[1], 0.0f));
    }

    /*
    * The hand jumping far (the mouse moved fast between two frames) reaches the cloth over several ticks at the hand's
    * speed, sliding through each tick's substeps, never in one go: the held point moves about as far in a tick as the
    * hand does, then arrives. Jumps of 35 cm in a substep crushed the fabric round the hand until self-collision's cut
    * let edges through.
    */
    void testAJumpIsSpreadOut() {
        Cloth cloth = standingSheet();
        pinParticle(cloth.particles, 20);
        pinParticle(cloth.particles, 24);
        const Vec3 start = cloth.particles.positions[12];
        const Vec3 target = start + Vec3{0.0f, 0.0f, 0.5f};
        const float reach = cloth.settings.handSpeed / 120.0f;

        cloth.grab = ClothGrab{12, target};
        stepCloth(cloth, 1.0f / 120.0f);
        assert(cloth.hand && cloth.hand->particle == 12);
        assert(near((cloth.hand->end - cloth.hand->start).length(), reach, 1e-5f));
        assert((cloth.particles.positions[12] - start).length() < 2.0f * reach);

        for (int tick = 0; tick < 60; ++tick) {
            stepCloth(cloth, 1.0f / 120.0f);
        }
        // There, held on the hand's spring, which the sheet taut between its pins stretches about 5.5 cm (measured).
        assert(nearVec(cloth.hand->end, target, 0.0f));
        assert(nearVec(cloth.particles.positions[12], target, 0.08f));

        cloth.grab.reset();
        stepCloth(cloth, 1.0f / 120.0f);
        assert(!cloth.hand);
    }

    // The sheet's momentum: its mass-weighted velocity, the velocity of its centre of mass.
    Vec3 centreOfMassVelocity(const ClothParticles& particles) {
        Vec3 momentum{0.0f, 0.0f, 0.0f};
        float mass = 0.0f;
        for (std::size_t index = 0; index < particles.size(); ++index) {
            const float pointMass = 1.0f / particles.inverseMasses[index];
            momentum = momentum + particles.velocities[index] * pointMass;
            mass += pointMass;
        }
        return momentum * (1.0f / mass);
    }

    /*
    * Let go, nothing stops the cloth, since velocity comes from motion: a free sheet towed through empty space (no
    * gravity, no drag) by one point at 1.2 m/s glides on. The towed point carries on forward, and the sheet's centre of
    * mass keeps the speed it had when let go (which sloshes around the towing speed while towed, the sheet bouncing on
    * the hand's spring), unchanged from tick to tick: its own edges push and pull its points equally and oppositely, so
    * they cannot change its momentum, but for the rounding floats drift over half a second: a velocity is a substep's
    * rounded motion over the substep, so twice the substeps drift twice as far (measured 0.16% of the glide at 15
    * substeps, 0.32% at 30).
    */
    void testLetGoPointsFlyOn() {
        ClothSettings settings;
        settings.gravity = 0.0f;
        settings.drag = 0.0f;
        Cloth cloth = makeCloth(Mesh::grid(4, 4, 2.0f, 2.0f), Mat4::identity(), settings);
        const Vec3 start = cloth.particles.positions[12];

        for (int tick = 0; tick < 240; ++tick) {
            cloth.grab = ClothGrab{12, start + Vec3{0.01f * tick, 0.0f, 0.0f}};
            stepCloth(cloth, 1.0f / 120.0f);
        }
        cloth.grab.reset();
        const Vec3 released = cloth.particles.positions[12];
        stepCloth(cloth, 1.0f / 120.0f);
        assert(cloth.particles.positions[12].x > released.x);
        const Vec3 gliding = centreOfMassVelocity(cloth.particles);
        assert(gliding.x > 0.5f);
        for (int tick = 0; tick < 60; ++tick) {
            stepCloth(cloth, 1.0f / 120.0f);
        }
        assert(nearVec(centreOfMassVelocity(cloth.particles), gliding, 0.01f));
    }
}

void testClothGrab() {
    testRaysFindTheNearestCorner();
    testGrabPullsToTheTarget();
    testGrabIsASpring();
    testHeldPointsFollow();
    testTheHandHasASpeedLimit();
    testAJumpIsSpreadOut();
    testLetGoPointsFlyOn();
}
