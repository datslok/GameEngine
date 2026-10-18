#include "math/transform.h"
#include "physics/character_controller.h"
#include "scene/collision_mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    constexpr float tick = 1.0f / 120.0f;
    constexpr float walkSpeed = 5.5f;

    float horizontalSpeed(const Vec3& velocity) {
        return std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    }

    // Runs the controller for the given time at the given tick, wishing the same velocity throughout.
    void run(CharacterController& controller, Vec3& feet, const StaticLevel& level, float seconds, const Vec3& wish,
             float step = tick) {
        const int ticks = static_cast<int>(std::lround(seconds / step));
        for (int i = 0; i < ticks; ++i) {
            controller.wishVelocity = wish;
            moveCharacter(controller, feet, level, step);
        }
    }

    // Lets a controller settle onto whatever is under it.
    void settle(CharacterController& controller, Vec3& feet, const StaticLevel& level) {
        run(controller, feet, level, 0.5f, Vec3{});
    }

    void testCapsule() {
        CharacterControllerSettings settings;
        settings.radius = 0.4f;
        settings.height = 1.8f;
        const Capsule capsule = controllerCapsule(settings, Vec3{1.0f, 2.0f, 3.0f});
        assert(near(capsule.a.y, 2.4f, 1e-5f) && near(capsule.b.y, 3.4f, 1e-5f));
        assert(near(capsule.a.x, 1.0f, 1e-5f) && near(capsule.b.z, 3.0f, 1e-5f));
        assert(near(capsule.radius, 0.4f, 1e-5f));
    }

    // Standing still: no creeping, on flat ground or on a walkable ramp.
    void testStandingStill() {
        const StaticLevel flat = flatLevel();
        CharacterController controller;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        run(controller, feet, flat, 10.0f, Vec3{});
        assert(near(feet.x, 0.0f, 1e-4f) && near(feet.y, 0.0f, 1e-3f) && near(feet.z, 0.0f, 1e-4f));
        assert(controller.grounded);
        assert(near(controller.groundNormal.y, 1.0f, 1e-4f));

        StaticLevel slope = flatLevel();
        addRamp(slope, 20.0f, 6.0f);
        CharacterController onRamp;
        Vec3 rampFeet{0.0f, 1.5f, 2.0f};
        settle(onRamp, rampFeet, slope);
        assert(onRamp.grounded);
        const Vec3 settled = rampFeet;
        run(onRamp, rampFeet, slope, 10.0f, Vec3{});
        assert(near(rampFeet.z, settled.z, 0.01f) && near(rampFeet.y, settled.y, 0.01f));
    }

    // Walls: stop a radius away when walking straight in; slide along at speed times cos(angle) when walking in at an angle.
    void testWalls() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{3.0f, 0.0f, -20.0f}, Vec3{4.0f, 3.0f, 20.0f}));

        CharacterController straight;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        run(straight, feet, level, 2.0f, Vec3{walkSpeed, 0.0f, 0.0f});
        assert(near(feet.x, 3.0f - straight.settings.radius, 0.01f));
        assert(near(feet.z, 0.0f, 1e-3f));
        assert(straight.grounded);

        CharacterController angled;
        Vec3 slide{0.0f, 0.0f, 0.0f};
        const float diagonal = walkSpeed / std::sqrt(2.0f);
        run(angled, slide, level, 1.0f, Vec3{diagonal, 0.0f, diagonal}); // reaches the wall within the first second
        const float zBefore = slide.z;
        run(angled, slide, level, 1.0f, Vec3{diagonal, 0.0f, diagonal});
        assert(near(slide.z - zBefore, diagonal, 0.1f));
        assert(slide.x <= 3.0f - angled.settings.radius + 0.01f);
    }

    // Into a corner: stopped by both walls, never pushed through either.
    void testCorner() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{3.0f, 0.0f, -20.0f}, Vec3{4.0f, 3.0f, 20.0f}));
        level.add(block(Vec3{-20.0f, 0.0f, 3.0f}, Vec3{20.0f, 3.0f, 4.0f}));

        CharacterController controller;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        const float diagonal = walkSpeed / std::sqrt(2.0f);
        run(controller, feet, level, 2.0f, Vec3{diagonal, 0.0f, diagonal});
        const float limit = 3.0f - controller.settings.radius;
        assert(near(feet.x, limit, 0.01f) && near(feet.z, limit, 0.01f));
        const Vec3 stopped = feet;
        run(controller, feet, level, 1.0f, Vec3{diagonal, 0.0f, diagonal});
        assert(near(feet.x, stopped.x, 0.005f) && near(feet.z, stopped.z, 0.005f));
    }

    // Ramps: a 20 degree one is walked up at walking speed along the surface; a 50 degree one cannot be climbed.
    void testRamps() {
        StaticLevel gentle = flatLevel();
        const float top = addRamp(gentle, 20.0f, 6.0f);
        CharacterController climber;
        Vec3 feet{0.0f, 0.0f, -2.0f};
        settle(climber, feet, gentle);

        // On the ramp, the distance covered along the surface in half a second is walking speed times half a second.
        run(climber, feet, gentle, 0.8f, Vec3{0.0f, 0.0f, walkSpeed}); // onto the ramp
        const Vec3 start = feet;
        run(climber, feet, gentle, 0.5f, Vec3{0.0f, 0.0f, walkSpeed});
        assert(near((feet - start).length(), walkSpeed * 0.5f, 0.1f));
        assert(feet.y > start.y + 0.5f);

        run(climber, feet, gentle, 1.0f, Vec3{0.0f, 0.0f, walkSpeed}); // onto the platform at the top, which is 6 long
        assert(near(feet.y, top, 0.02f));
        assert(climber.grounded);

        StaticLevel steep = flatLevel();
        addRamp(steep, 50.0f, 4.0f);
        CharacterController blocked;
        Vec3 steepFeet{0.0f, 0.0f, -2.0f};
        settle(blocked, steepFeet, steep);
        run(blocked, steepFeet, steep, 3.0f, Vec3{0.0f, 0.0f, walkSpeed});
        assert(steepFeet.y < 0.5f);
    }

    // Stairs of 0.25 are climbed (the step height is 0.3); a 0.5 ledge is a wall. Walking back down, the character stays
    // on the ground every tick (snapping) instead of hopping from step to step.
    void testSteps() {
        StaticLevel stairs = flatLevel();
        for (int step = 0; step < 5; ++step) {
            const float z = 1.0f + 0.5f * static_cast<float>(step);
            stairs.add(block(Vec3{-2.0f, 0.0f, z}, Vec3{2.0f, 0.25f * static_cast<float>(step + 1), 10.0f}));
        }

        CharacterController climber;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        settle(climber, feet, stairs);
        run(climber, feet, stairs, 1.2f, Vec3{0.0f, 0.0f, walkSpeed}); // up onto the top step, which runs on to z = 10
        assert(near(feet.y, 1.25f, 0.02f));
        assert(feet.z > 4.0f);

        // Back down: grounded on every single tick.
        for (int i = 0; i < 240; ++i) {
            climber.wishVelocity = Vec3{0.0f, 0.0f, -walkSpeed};
            moveCharacter(climber, feet, stairs, tick);
            assert(climber.grounded);
        }
        assert(near(feet.y, 0.0f, 0.02f));

        // A low kerb (0.1, like the demo course's floor slab, whose underside lies on the floor) is walked straight onto,
        // by a wider capsule that starts and stops at once (the demo's duck), and it ends up standing on top.
        StaticLevel kerb = flatLevel();
        kerb.add(block(Vec3{14.0f, 0.0f, -8.0f}, Vec3{20.0f, 0.1f, 8.0f}));
        CharacterController duck;
        duck.settings.radius = 0.5f;
        duck.settings.groundAcceleration = 1000.0f;
        Vec3 duckFeet{12.0f, 0.0f, 0.0f};
        run(duck, duckFeet, kerb, 1.0f, Vec3{6.0f, 0.0f, 0.0f});
        assert(duckFeet.x > 17.0f);
        assert(near(duckFeet.y, 0.1f, 1e-3f));

        StaticLevel ledge = flatLevel();
        ledge.add(block(Vec3{-2.0f, 0.0f, 1.0f}, Vec3{2.0f, 0.5f, 5.0f}));
        CharacterController stopped;
        Vec3 ledgeFeet{0.0f, 0.0f, 0.0f};
        run(stopped, ledgeFeet, ledge, 2.0f, Vec3{0.0f, 0.0f, walkSpeed});
        assert(near(ledgeFeet.y, 0.0f, 0.01f));
        assert(ledgeFeet.z <= 1.0f - stopped.settings.radius + 0.01f);
    }

    // A jump rises to v^2 / 2g and lands after 2v / g (Overwatch's launch speed and gravity by default), and a ceiling
    // cuts it short.
    void testJumping() {
        const StaticLevel flat = flatLevel();
        CharacterController jumper;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        settle(jumper, feet, flat);

        const float launch = jumper.settings.jumpSpeed;
        const float gravity = jumper.settings.gravity;
        jumper.wantsJump = true;
        float highest = 0.0f;
        int airborneTicks = 0;
        for (int i = 0; i < 240; ++i) {
            moveCharacter(jumper, feet, flat, tick);
            highest = std::max(highest, feet.y);
            if (!jumper.grounded) {
                ++airborneTicks;
            }
            assert(!jumper.wantsJump); // the command is used up by the tick that sees it
        }
        // Semi-implicit Euler moves with the velocity at the end of each tick, so it peaks about launch * tick / 2 low.
        assert(near(highest, launch * launch / (2.0f * gravity), launch * tick / 2.0f + 0.01f));
        assert(near(static_cast<float>(airborneTicks) * tick, 2.0f * launch / gravity, 0.03f));
        assert(jumper.grounded && near(feet.y, 0.0f, 1e-3f));

        // No jumping in mid-air: a jump wished while falling does nothing.
        CharacterController falling;
        Vec3 high{0.0f, 5.0f, 0.0f};
        falling.wantsJump = true;
        moveCharacter(falling, high, flat, tick);
        assert(falling.velocity.y <= 0.0f);

        // A ceiling 0.3 above the head: the jump stops there and comes back down.
        StaticLevel low = flatLevel();
        const float headroom = jumper.settings.height + 0.3f;
        low.add(block(Vec3{-5.0f, headroom, -5.0f}, Vec3{5.0f, headroom + 1.0f, 5.0f}));
        CharacterController bonk;
        Vec3 under{0.0f, 0.0f, 0.0f};
        settle(bonk, under, low);
        bonk.wantsJump = true;
        float peak = 0.0f;
        for (int i = 0; i < 120; ++i) {
            moveCharacter(bonk, under, low, tick);
            peak = std::max(peak, under.y);
        }
        assert(peak <= 0.3f + 0.01f);
        assert(bonk.grounded && near(under.y, 0.0f, 1e-3f));
    }

    // Falling: quadratic air drag gives v(t) = 30 tanh(g t / 30), and a fast fall onto a thin floor does not pass through it.
    void testFalling() {
        const StaticLevel nothing;
        CharacterController faller;
        Vec3 feet{0.0f, 100.0f, 0.0f};
        run(faller, feet, nothing, 1.0f, Vec3{});
        const float terminal = faller.settings.maxFallSpeed;
        const float expected = terminal * std::tanh(faller.settings.gravity / terminal);
        assert(near(-faller.velocity.y, expected, 0.2f));
        run(faller, feet, nothing, 9.0f, Vec3{});
        assert(near(-faller.velocity.y, terminal, 0.1f));
        assert(-faller.velocity.y <= terminal);

        StaticLevel thin;
        thin.add(block(Vec3{-5.0f, -0.05f, -5.0f}, Vec3{5.0f, 0.0f, 5.0f}));
        CharacterController diver;
        Vec3 drop{0.0f, 20.0f, 0.0f};
        run(diver, drop, thin, 4.0f, Vec3{});
        assert(diver.grounded && near(drop.y, 0.0f, 1e-3f));
    }

    // In the air: with no input the velocity is kept (momentum); steering up to 90 degrees turns it without slowing it,
    // the way Overwatch's 2018 movement update describes.
    void testAirControl() {
        const StaticLevel flat = flatLevel();
        CharacterController runner;
        Vec3 feet{0.0f, 0.0f, 0.0f};
        settle(runner, feet, flat);
        run(runner, feet, flat, 0.5f, Vec3{0.0f, 0.0f, walkSpeed});

        runner.wantsJump = true;
        run(runner, feet, flat, 0.1f, Vec3{});
        assert(!runner.grounded);
        assert(near(runner.velocity.z, walkSpeed, 0.01f) && near(runner.velocity.x, 0.0f, 0.01f));

        for (int i = 0; i < 24; ++i) {
            runner.wishVelocity = Vec3{walkSpeed, 0.0f, 0.0f};
            moveCharacter(runner, feet, flat, tick);
            assert(horizontalSpeed(runner.velocity) >= walkSpeed - 0.01f);
        }
        assert(runner.velocity.x > 1.0f);
    }

    // The same walk at 60 and at 120 ticks a second ends in nearly the same place.
    void testTickRate() {
        StaticLevel level = flatLevel();
        addRamp(level, 20.0f, 6.0f);
        CharacterController fast;
        CharacterController slow;
        Vec3 fastFeet{0.5f, 0.0f, -3.0f};
        Vec3 slowFeet = fastFeet;
        run(fast, fastFeet, level, 2.0f, Vec3{0.0f, 0.0f, walkSpeed}, 1.0f / 120.0f);
        run(slow, slowFeet, level, 2.0f, Vec3{0.0f, 0.0f, walkSpeed}, 1.0f / 60.0f);
        assert((fastFeet - slowFeet).length() < 0.05f);
    }

    // Whether a capsule would fit somewhere (what standing up from a crouch needs to know): standing on the floor fits,
    // under a low roof or poking into a wall does not.
    void testFits() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{2.0f, 1.5f, -5.0f}, Vec3{10.0f, 2.5f, 5.0f}));
        const CharacterControllerSettings standing;
        assert(characterFits(standing, Vec3{0.0f, 0.0f, 0.0f}, level));
        assert(!characterFits(standing, Vec3{4.0f, 0.0f, 0.0f}, level));
        CharacterControllerSettings crouched;
        crouched.height = 1.2f;
        assert(characterFits(crouched, Vec3{4.0f, 0.0f, 0.0f}, level));
        assert(!characterFits(standing, Vec3{0.0f, -0.5f, 0.0f}, level));
    }

    // The system moves every entity with a controller and a Transform.
    void testSystem() {
        const StaticLevel flat = flatLevel();
        World world;
        const Entity character = world.create();
        world.add(character, Transform{});
        CharacterController controller;
        controller.wishVelocity = Vec3{walkSpeed, 0.0f, 0.0f};
        world.add(character, controller);

        for (int i = 0; i < 120; ++i) {
            moveCharacters(world, flat, tick);
        }
        // A second of walking, less the bit lost while speeding up from rest (about walkSpeed^2 / 2a).
        assert(near(world.get<Transform>(character).position.x, walkSpeed, 0.2f));
        assert(world.get<CharacterController>(character).grounded);
    }
}

void testCharacterController() {
    testCapsule();
    testStandingStill();
    testWalls();
    testCorner();
    testRamps();
    testSteps();
    testJumping();
    testFalling();
    testAirControl();
    testTickRate();
    testFits();
    testSystem();
}
