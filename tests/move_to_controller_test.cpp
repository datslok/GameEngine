#include "gameplay/move_to_controller.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }
}

/*
* The move-to controller only decides the velocity that heads for the target; something else (the character controller)
* moves the character with it.
*/
void testMoveToController() {
    MoveToController movement{2.0f};
    const Vec3 start{0.0f, 1.0f, 0.0f};

    assert(!movement.isMoving());
    assertVector(movement.desiredVelocity(start, 1.0f), Vec3{});

    // A 3-4-5 triangle: full speed (2) along the ground towards the target, whatever the height difference.
    movement.setTarget(Vec3{3.0f, 0.0f, 4.0f});
    assertVector(movement.desiredVelocity(start, 1.0f), Vec3{1.2f, 0.0f, 1.6f});
    assert(movement.isMoving());

    // A step longer than the remaining distance asks only for what lands exactly on the target.
    assertVector(movement.desiredVelocity(start, 10.0f), Vec3{0.3f, 0.0f, 0.4f});

    // Arrival: still moving until the character is (as good as) there, then stopped.
    movement.updateArrival(Vec3{2.9f, 1.0f, 3.9f});
    assert(movement.isMoving());
    movement.updateArrival(Vec3{3.0f, 7.0f, 4.0f});
    assert(!movement.isMoving());
    assertVector(movement.desiredVelocity(Vec3{3.0f, 7.0f, 4.0f}, 1.0f), Vec3{});

    // A new command replaces the old destination.
    movement.setTarget(Vec3{10.0f, 0.0f, 0.0f});
    movement.setTarget(Vec3{0.0f, 0.0f, -10.0f});
    assertVector(movement.desiredVelocity(start, 1.0f), Vec3{0.0f, 0.0f, -2.0f});

    movement.stop();
    assert(!movement.isMoving());
    assertVector(movement.desiredVelocity(start, 1.0f), Vec3{});

    // Already at the destination, or no time to move in: no division by zero.
    movement.setTarget(start);
    assertVector(movement.desiredVelocity(start, 1.0f), Vec3{});
    movement.setTarget(Vec3{5.0f, 0.0f, 0.0f});
    assertVector(movement.desiredVelocity(start, 0.0f), Vec3{});
}
