#include "fixed_timestep.h"
#include "scene.h"
#include "transform.h"
#include "vec4.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
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
        to.rotation.y = 1.0f;
        to.scale = Vec3{3.0f, 3.0f, 3.0f};

        const Transform halfway = interpolate(from, to, 0.5f);

        assert(nearlyEqual(halfway.position.x, 5.0));
        assert(nearlyEqual(halfway.position.y, -1.0));
        assert(nearlyEqual(halfway.position.z, 2.0));
        assert(nearlyEqual(halfway.rotation.y, 0.5));
        assert(nearlyEqual(halfway.scale.x, 2.0));

        const Transform start = interpolate(from, to, 0.0f);
        const Transform end = interpolate(from, to, 1.0f);

        assert(nearlyEqual(start.position.x, 0.0));
        assert(nearlyEqual(end.position.x, 10.0));
    }

    // An angle that wraps past 2 pi takes the short way round instead of spinning backwards.
    {
        constexpr float fullTurn = 2.0f * std::numbers::pi_v<float>;

        Transform from;
        from.rotation.y = fullTurn - 0.1f;

        Transform to;
        to.rotation.y = 0.1f;

        const Transform halfway = interpolate(from, to, 0.5f);

        // Halfway is a full turn (the same as 0), not pi.
        const float wrapped = std::remainder(halfway.rotation.y, fullTurn);
        assert(nearlyEqual(wrapped, 0.0));
    }

    // Objects added to a scene start with no motion to interpolate.
    {
        Scene scene;
        MeshInstance object{std::make_shared<Mesh>(Mesh::cube())};
        object.transform.position = Vec3{5.0f, 0.0f, 0.0f};

        scene.add(object);
        assert(nearlyEqual(scene.getObjects().at(0).previousTransform.position.x, 5.0));

        // After a tick moves it, previous holds the old position.
        scene.savePreviousTransforms();
        scene.getObjects().at(0).transform.position.x = 6.0f;

        const MeshInstance& moved = scene.getObjects().at(0);
        assert(nearlyEqual(moved.previousTransform.position.x, 5.0));

        const Vec4 halfway = moved.getInterpolatedModelMatrix(0.5f) * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        assert(nearlyEqual(halfway.x, 5.5));
    }
}
