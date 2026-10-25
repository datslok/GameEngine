#include "core/fixed_timestep.h"
#include "scene/interpolation.h"
#include "math/transform.h"
#include "math/quat.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
    bool nearlyEqual(double actual, double expected) {
        return std::abs(actual - expected) < 0.00001;
    }

    bool throwsInvalidArgument(double ticksPerSecond, double maxFrameSeconds) {
        try {
            FixedTimestep timestep{ticksPerSecond, maxFrameSeconds};
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }
}

void testFixedTimestep() {
    // 10 ticks per second keeps the numbers easy to follow: one tick is 0.1 s.
    {
        FixedTimestep timestep{10.0, 1.0};
        assert(nearlyEqual(timestep.getTickSeconds(), 0.1));

        // Less than a tick: nothing to simulate yet, but the time is kept.
        assert(timestep.advance(0.04) == 0);
        assert(nearlyEqual(timestep.getAlpha(), 0.4));

        // 0.04 + 0.07 = 0.11: one tick, 0.01 left over.
        assert(timestep.advance(0.07) == 1);
        assert(nearlyEqual(timestep.getAlpha(), 0.1));

        // A slow frame runs several ticks.
        assert(timestep.advance(0.25) == 2);
        assert(nearlyEqual(timestep.getAlpha(), 0.6));
    }

    // Many small frames add up to the same number of ticks as one big frame.
    {
        FixedTimestep timestep{10.0, 1.0};
        int ticks = 0;

        // 24 * 0.02 = 0.48. Avoid landing exactly on a tick boundary, where rounding decides the count.
        for (int frame = 0; frame < 24; ++frame) {
            ticks += timestep.advance(0.02);
        }

        assert(ticks == 4);
        assert(nearlyEqual(timestep.getAlpha(), 0.8));
    }

    // A hitch is clamped, so the game slows down instead of running 50 catch-up ticks.
    {
        FixedTimestep timestep{10.0, 0.25};

        assert(timestep.advance(5.0) == 2);
        assert(nearlyEqual(timestep.getFrameSeconds(), 0.25));
        assert(nearlyEqual(timestep.getAlpha(), 0.5));
    }

    // A clock going backwards or returning garbage is treated as no time passing.
    {
        FixedTimestep timestep{10.0, 1.0};

        assert(timestep.advance(-1.0) == 0);
        assert(timestep.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
        assert(nearlyEqual(timestep.getAlpha(), 0.0));
    }

    // The tick budget: a frame always runs its first tick, and another only while the ticks so far, plus one more of
    // their average cost, fit in the budget.
    {
        assert(fitsTickBudget(0, 0.0, 0.016));
        assert(fitsTickBudget(0, 0.0, 0.0));

        // Cheap ticks catch up after a hitch: 0.3 ms each, 12 run, a 13th still fits in 16 ms.
        assert(fitsTickBudget(12, 0.0036, 0.016));

        // The overloaded big sheet: 11.4 ms a tick, a second would end at 22.8 ms.
        assert(!fitsTickBudget(1, 0.0114, 0.016));

        // Exactly on the budget still fits.
        assert(fitsTickBudget(3, 0.012, 0.016));
        assert(!fitsTickBudget(3, 0.0121, 0.016));

        // 0 means no budget: only maxFrameSeconds limits the ticks.
        assert(fitsTickBudget(30, 0.3, 0.0));
    }

    // Ticks a frame drops are gone: the game falls behind real time instead of owing them, and the next frame's alpha
    // comes from the time left over as usual.
    {
        FixedTimestep timestep{10.0, 1.0};
        assert(timestep.advance(0.55) == 5);

        // The loop runs only one of the five; the next frame owes nothing more for them.
        assert(timestep.advance(0.04) == 0);
        assert(nearlyEqual(timestep.getAlpha(), 0.9));
    }

    // The clamp must allow at least one tick, or a slow frame could never produce one.
    assert(throwsInvalidArgument(0.0, 1.0));
    assert(throwsInvalidArgument(10.0, 0.05));
    assert(!throwsInvalidArgument(120.0, 0.25));

    // Interpolating transforms between two ticks.
    {
        Transform from;
        from.position = Vec3{0.0f, 0.0f, 0.0f};
        from.scale = Vec3{1.0f, 1.0f, 1.0f};

        Transform to;
        to.position = Vec3{10.0f, -2.0f, 4.0f};
        to.rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 1.0f);
        to.scale = Vec3{3.0f, 3.0f, 3.0f};

        const Transform halfway = interpolate(from, to, 0.5f);

        assert(nearlyEqual(halfway.position.x, 5.0));
        assert(nearlyEqual(halfway.position.y, -1.0));
        assert(nearlyEqual(halfway.position.z, 2.0));
        assert(angleBetween(halfway.rotation, Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.5f)) < 0.0001f);
        assert(nearlyEqual(halfway.scale.x, 2.0));

        const Transform start = interpolate(from, to, 0.0f);
        const Transform end = interpolate(from, to, 1.0f);

        assert(nearlyEqual(start.position.x, 0.0));
        assert(nearlyEqual(end.position.x, 10.0));
    }

    // A turn that wraps past 2 pi takes the short way round instead of spinning backwards.
    {
        constexpr float fullTurn = 2.0f * std::numbers::pi_v<float>;

        Transform from;
        from.rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, fullTurn - 0.1f);

        Transform to;
        to.rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.1f);

        // Halfway is a full turn (the same as no turn), not a half turn.
        const Transform halfway = interpolate(from, to, 0.5f);
        assert(angleBetween(halfway.rotation, Quat{}) < 0.0001f);
    }

    // A tumble about a tilted axis is drawn halfway along that same turn. Blending three Euler angles separately could not
    // do this: their in-between orientation is off the turn's path.
    {
        const Vec3 tiltedAxis = Vec3{1.0f, 1.0f, 1.0f}.normalized();

        Transform from;
        Transform to;
        to.rotation = Quat::fromAxisAngle(tiltedAxis, 1.2f);

        const Transform halfway = interpolate(from, to, 0.5f);
        assert(angleBetween(halfway.rotation, Quat::fromAxisAngle(tiltedAxis, 0.6f)) < 0.0001f);
    }

    // Entities with a PreviousTransform are drawn between the last two ticks.
    {
        World world;
        Transform start;
        start.position = Vec3{5.0f, 0.0f, 0.0f};

        const Entity moving = world.create();
        world.add(moving, start);
        world.add(moving, PreviousTransform{start});

        // A tick saves the old transform, then moves the entity.
        savePreviousTransforms(world);
        world.get<Transform>(moving).position.x = 6.0f;

        assert(nearlyEqual(world.get<PreviousTransform>(moving).transform.position.x, 5.0));
        assert(nearlyEqual(getRenderTransform(world, moving, 0.0f).position.x, 5.0));
        assert(nearlyEqual(getRenderTransform(world, moving, 0.5f).position.x, 5.5));
        assert(nearlyEqual(getRenderTransform(world, moving, 1.0f).position.x, 6.0));

        // Without a PreviousTransform an entity is drawn exactly where it is, so teleports do not slide.
        const Entity teleporting = world.create();
        world.add(teleporting, start);
        world.get<Transform>(teleporting).position.x = 20.0f;
        assert(nearlyEqual(getRenderTransform(world, teleporting, 0.5f).position.x, 20.0));
    }
}
