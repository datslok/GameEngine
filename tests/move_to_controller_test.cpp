#include "move_to_controller.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }
}

void testMoveToController() {
    MoveToController movement{2.0f};

    Vec3 position{0.0f, 1.0f, 0.0f};

    assert(!movement.isMoving());

    // A 3-4-5 triangle: two units of movement in one second.
    movement.setTarget(Vec3{3.0f, 0.0f, 4.0f});
    movement.update(position, 1.0f);

    assert(nearlyEqual(position.x, 1.2f));
    assert(nearlyEqual(position.y, 1.0f));
    assert(nearlyEqual(position.z, 1.6f));
    assert(movement.isMoving());

    // A large step must stop at the target without overshooting.
    movement.update(position, 10.0f);

    assert(nearlyEqual(position.x, 3.0f));
    assert(nearlyEqual(position.y, 1.0f));
    assert(nearlyEqual(position.z, 4.0f));
    assert(!movement.isMoving());

    // A new command replaces the old destination.
    position = Vec3{0.0f, 1.0f, 0.0f};

    movement.setTarget(Vec3{10.0f, 0.0f, 0.0f});
    movement.setTarget(Vec3{0.0f, 0.0f, -10.0f});
    movement.update(position, 1.0f);

    assert(nearlyEqual(position.x, 0.0f));
    assert(nearlyEqual(position.z, -2.0f));

    movement.stop();
    movement.update(position, 1.0f);

    assert(nearlyEqual(position.z, -2.0f));

    // Splitting the same elapsed time should give the same result.
    MoveToController wholeStep{2.0f};
    MoveToController splitStep{2.0f};

    Vec3 a{0.0f, 1.0f, 0.0f};
    Vec3 b = a;

    const Vec3 destination{10.0f, 0.0f, 10.0f};

    wholeStep.setTarget(destination);
    splitStep.setTarget(destination);

    wholeStep.update(a, 1.0f);

    for (int step = 0; step < 4; ++step) {
        splitStep.update(b, 0.25f);
    }

    assert(nearlyEqual(a.x, b.x));
    assert(nearlyEqual(a.y, b.y));
    assert(nearlyEqual(a.z, b.z));

    // Already at the destination: no division by zero.
    movement.setTarget(position);
    movement.update(position, 0.0f);

    assert(!movement.isMoving());
}