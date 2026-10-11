#include "gameplay/firefly.h"
#include "math/transform.h"

#include <cmath>

namespace {
    // How fast each wobble swings, in radians per second. The ratios are irrational-looking on purpose: wobbles that never
    // line up with each other or with the orbit make the path never quite repeat, so it reads as wandering, not a machine.
    constexpr double radiusWobbleRate = 0.83;
    constexpr double heightWobbleRate = 1.71;
    constexpr double surgeRate = 1.29;

    // The orbit speeds up and slows down (sometimes briefly backing up) by this much angle, so it darts instead of gliding.
    constexpr double surgeAngle = 0.5;
}

/*
* Computed from the total time, like the spinners, instead of stepped each tick, so rounding errors cannot build up and the
* position for any moment is known in advance (the demo uses that to place a lamp the moment it is switched on).
* Each coordinate is a sum of sines, a Lissajous-like figure: bounded and smooth by construction. Time stays a double until
* the end, because sin of a large float loses precision after a few hours.
*/
Vec3 fireflyPosition(const Firefly& firefly, double seconds) {
    const double phase = firefly.phase;

    const double angle = phase + firefly.orbitSpeed * seconds + surgeAngle * std::sin(surgeRate * seconds + 2.0 * phase);
    const double radius = firefly.orbitRadius + firefly.radiusWobble * std::sin(radiusWobbleRate * seconds + 3.0 * phase);
    const double height = firefly.height + firefly.heightWobble * std::sin(heightWobbleRate * seconds + 5.0 * phase);

    return firefly.centre + Vec3{
        static_cast<float>(radius * std::cos(angle)),
        static_cast<float>(height),
        static_cast<float>(radius * std::sin(angle))
    };
}

void updateFireflies(World& world, double simulationSeconds) {
    world.each<Firefly, Transform>([simulationSeconds](Entity, Firefly& firefly, Transform& transform) {
        transform.position = fireflyPosition(firefly, simulationSeconds);
    });
}
