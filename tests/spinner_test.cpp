#include "gameplay/spinner.h"
#include "math/quat.h"
#include "math/transform.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    bool sameRotation(const Quat& actual, const Quat& expected) {
        return angleBetween(actual, expected) < 0.0001f;
    }
}

void testSpinner() {
    World world;
    const Spinner spinner{Vec3{0.3f, 0.0f, 0.0f}, Vec3{2.0f, -1.0f, 0.0f}};

    Transform placement;
    placement.position = Vec3{1.0f, 2.0f, 3.0f};

    const Entity entity = world.create();
    world.add(entity, placement);
    world.add(entity, spinner);

    // At time 0 it shows its starting angles.
    updateSpinners(world, 0.0);
    assert(sameRotation(world.get<Transform>(entity).rotation, Quat::fromEulerAngles(Vec3{0.3f, 0.0f, 0.0f})));

    // Each angle grows at its own speed and the three are combined in the engine's x, y, z order. The position stays.
    updateSpinners(world, 1.5);
    assert(sameRotation(world.get<Transform>(entity).rotation, Quat::fromEulerAngles(Vec3{0.3f + 3.0f, -1.5f, 0.0f})));
    assert(world.get<Transform>(entity).position.x == 1.0f);

    // Computed from the total time, not added up tick by tick, so it is still exact after a long session (a day, in
    // seconds, is far past the precision of a float angle).
    const double aDay = 86400.0;
    const double fullTurn = 2.0 * std::numbers::pi;
    const double angleX = 0.3 + 2.0 * aDay;
    const double angleY = -1.0 * aDay;
    updateSpinners(world, aDay);
    assert(sameRotation(world.get<Transform>(entity).rotation,
        Quat::fromEulerAngles(Vec3{static_cast<float>(std::fmod(angleX, fullTurn)),
                                   static_cast<float>(std::fmod(angleY, fullTurn)), 0.0f})));
}
