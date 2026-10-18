#include "gameplay/first_person_body.h"
#include "math/transform.h"
#include "scene/collision_mesh.h"
#include "scene/interpolation.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    constexpr float tick = 1.0f / 120.0f;
    constexpr float pi = std::numbers::pi_v<float>;

    // WASD as axes, like getMoveAxes: x is +1 for right (D), y is +1 for forward (W).
    Vec3 wish(float right, float forward, float yaw = 0.0f, bool crouching = false, bool sprinting = false) {
        return firstPersonWishVelocity(Vec2{right, forward}, yaw, crouching, sprinting, FirstPersonMoveSettings{});
    }

    float speed(const Vec3& velocity) {
        return velocity.length();
    }

    // Overwatch's measured speeds, for every key combination. Facing yaw 0 is looking down -z, so forward is -z and right +x.
    void testWishVelocities() {
        const FirstPersonMoveSettings settings;

        // Forward, sideways and the forward diagonals: full walking speed, and a diagonal is not faster (it is normalised,
        // or W+D would be sqrt(2) times as fast).
        const Vec3 forward = wish(0.0f, 1.0f);
        assert(near(forward.x, 0.0f, 1e-5f) && near(forward.z, -settings.walkSpeed, 1e-5f) && forward.y == 0.0f);
        const Vec3 right = wish(1.0f, 0.0f);
        assert(near(right.x, settings.walkSpeed, 1e-5f) && near(right.z, 0.0f, 1e-5f));
        assert(near(speed(wish(-1.0f, 0.0f)), settings.walkSpeed, 1e-5f));
        const Vec3 diagonal = wish(1.0f, 1.0f);
        assert(near(speed(diagonal), settings.walkSpeed, 1e-5f));
        assert(near(diagonal.x, -diagonal.z, 1e-5f) && diagonal.x > 0.0f);

        // Backwards is 90% (4.95), the backward diagonals 93.3% (5.13).
        assert(near(speed(wish(0.0f, -1.0f)), 4.95f, 1e-3f));
        assert(near(wish(0.0f, -1.0f).z, 4.95f, 1e-3f));
        assert(near(speed(wish(1.0f, -1.0f)), 5.133f, 1e-3f));
        assert(near(speed(wish(-1.0f, -1.0f)), 5.133f, 1e-3f));

        // Crouching is 3 in every direction, sprinting multiplies (but not while crouched), no keys is no motion.
        assert(near(speed(wish(0.0f, -1.0f, 0.0f, true)), settings.crouchSpeed, 1e-5f));
        assert(near(speed(wish(1.0f, 1.0f, 0.0f, true)), settings.crouchSpeed, 1e-5f));
        assert(near(speed(wish(0.0f, 1.0f, 0.0f, false, true)), settings.walkSpeed * settings.sprintMultiplier, 1e-4f));
        assert(near(speed(wish(0.0f, 1.0f, 0.0f, true, true)), settings.crouchSpeed, 1e-5f));
        assert(speed(wish(0.0f, 0.0f)) == 0.0f);

        // Facing turned a quarter right (yaw pi / 2 looks down +x): forward is now +x.
        const Vec3 turned = wish(0.0f, 1.0f, pi / 2.0f);
        assert(near(turned.x, settings.walkSpeed, 1e-4f) && near(turned.z, 0.0f, 1e-4f));
    }

    // Walking: the body moves along the wished direction, and the eyes sit at eye height above its interpolated feet.
    void testWalkingAndEyes() {
        const StaticLevel level = flatLevel();
        World world;
        const Entity body = spawnFirstPersonBody(world, Vec3{0.0f, 0.0f, 0.0f});
        world.get<CharacterController>(body).wishVelocity = wish(0.0f, 1.0f);

        for (int i = 0; i < 120; ++i) {
            savePreviousTransforms(world);
            updateFirstPersonBodies(world, level, tick);
        }
        const Vec3 feet = world.get<Transform>(body).position;
        assert(near(feet.z, -5.5f, 0.2f) && near(feet.x, 0.0f, 1e-4f) && near(feet.y, 0.0f, 1e-3f));

        // Halfway between the last two ticks, the eyes are halfway between, at standing eye height.
        const Vec3 before = world.get<PreviousTransform>(body).transform.position;
        const Vec3 eye = firstPersonEyePosition(world, body, 0.5f);
        const FirstPersonMoveSettings settings;
        assert(near(eye.z, (before.z + feet.z) * 0.5f, 1e-4f));
        assert(near(eye.y, settings.standingEyeHeight, 1e-4f));
    }

    // Crouching shrinks the capsule and eases the eyes down; standing back up needs room above.
    void testCrouching() {
        StaticLevel level = flatLevel();
        // A low roof, 1.5 above the ground, over x > 2.
        level.add(block(Vec3{2.0f, 1.5f, -5.0f}, Vec3{10.0f, 2.5f, 5.0f}));
        const FirstPersonMoveSettings settings;

        World world;
        const Entity body = spawnFirstPersonBody(world, Vec3{0.0f, 0.0f, 0.0f});
        FirstPersonBody& crouch = world.get<FirstPersonBody>(body);
        crouch.wantsCrouch = true;
        updateFirstPersonBodies(world, level, tick);
        assert(crouch.crouching);
        assert(near(world.get<CharacterController>(body).settings.height, settings.crouchingHeight, 1e-5f));

        // The eyes ease down rather than snapping: part way after one tick, all the way within a fifth of a second.
        assert(crouch.eyeHeight < settings.standingEyeHeight && crouch.eyeHeight > settings.crouchingEyeHeight);
        for (int i = 0; i < 24; ++i) {
            updateFirstPersonBodies(world, level, tick);
        }
        assert(near(crouch.eyeHeight, settings.crouchingEyeHeight, 1e-5f));

        // Crouch-walk under the roof and let go of crouch: no room to stand, so it stays crouched.
        world.get<CharacterController>(body).wishVelocity = Vec3{settings.crouchSpeed, 0.0f, 0.0f};
        for (int i = 0; i < 180; ++i) {
            updateFirstPersonBodies(world, level, tick);
        }
        assert(world.get<Transform>(body).position.x > 3.0f);
        crouch.wantsCrouch = false;
        world.get<CharacterController>(body).wishVelocity = Vec3{};
        updateFirstPersonBodies(world, level, tick);
        assert(crouch.crouching);

        // Back out from under it, and it stands up.
        world.get<CharacterController>(body).wishVelocity = Vec3{-settings.crouchSpeed, 0.0f, 0.0f};
        for (int i = 0; i < 240; ++i) {
            updateFirstPersonBodies(world, level, tick);
        }
        assert(!crouch.crouching);
        assert(near(world.get<CharacterController>(body).settings.height, settings.standingHeight, 1e-5f));
    }

    // Placing the body somewhere new is a teleport: no interpolation streak, no speed carried along, and it finds out
    // afresh whether it stands on anything. A jump command then leaves the ground.
    void testPlacingAndJumping() {
        const StaticLevel level = flatLevel();
        World world;
        const Entity body = spawnFirstPersonBody(world, Vec3{0.0f, 0.0f, 0.0f});
        world.get<CharacterController>(body).velocity = Vec3{3.0f, 0.0f, 0.0f};

        placeFirstPersonBody(world, body, Vec3{5.0f, 10.0f, 5.0f});
        assert(world.get<Transform>(body).position.y == 10.0f);
        assert(world.get<PreviousTransform>(body).transform.position.y == 10.0f);
        assert(world.get<CharacterController>(body).velocity.lengthSquared() == 0.0f);
        assert(!world.get<CharacterController>(body).groundKnown);

        // It falls onto the floor, then jumps.
        for (int i = 0; i < 360; ++i) {
            updateFirstPersonBodies(world, level, tick);
        }
        assert(world.get<CharacterController>(body).grounded);
        assert(near(world.get<Transform>(body).position.y, 0.0f, 1e-3f));
        world.get<CharacterController>(body).wantsJump = true;
        updateFirstPersonBodies(world, level, tick);
        assert(!world.get<CharacterController>(body).grounded);
        assert(world.get<Transform>(body).position.y > 0.0f);
    }
}

void testFirstPersonBody() {
    testWishVelocities();
    testWalkingAndEyes();
    testCrouching();
    testPlacingAndJumping();
}
