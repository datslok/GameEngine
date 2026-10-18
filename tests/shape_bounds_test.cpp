#include "physics/shapes.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return std::abs(actual.x - expected.x) < 0.0001f
            && std::abs(actual.y - expected.y) < 0.0001f
            && std::abs(actual.z - expected.z) < 0.0001f;
    }
}

void testShapeBounds() {
    // A sphere: its centre plus and minus the radius on every axis.
    const Aabb ball = boundsOf(Shape{Sphere{Vec3{1.0f, 2.0f, 3.0f}, 0.5f}});
    assert(nearlyEqual(ball.minimum, Vec3{0.5f, 1.5f, 2.5f}));
    assert(nearlyEqual(ball.maximum, Vec3{1.5f, 2.5f, 3.5f}));

    // A slanted capsule: the box around both end balls.
    const Aabb pill = boundsOf(Shape{Capsule{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, -1.0f, 0.0f}, 0.5f}});
    assert(nearlyEqual(pill.minimum, Vec3{-0.5f, -1.5f, -0.5f}));
    assert(nearlyEqual(pill.maximum, Vec3{2.5f, 0.5f, 0.5f}));

    // A box is its own bounds.
    const Aabb crate{Vec3{-1.0f, 0.0f, 2.0f}, Vec3{1.0f, 3.0f, 4.0f}};
    const Aabb same = boundsOf(Shape{crate});
    assert(nearlyEqual(same.minimum, crate.minimum));
    assert(nearlyEqual(same.maximum, crate.maximum));
}
