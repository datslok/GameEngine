#include "physics/triangle_contact.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    // A floor triangle at y = 0, counterclockwise seen from above (front face up), and a wall triangle in the x = 2 plane.
    const Vec3 floorA{-5.0f, 0.0f, 5.0f};
    const Vec3 floorB{5.0f, 0.0f, 5.0f};
    const Vec3 floorC{0.0f, 0.0f, -5.0f};
    const Vec3 wallA{2.0f, -1.0f, -3.0f};
    const Vec3 wallB{2.0f, -1.0f, 3.0f};
    const Vec3 wallC{2.0f, 4.0f, 0.0f};
}

void testTriangleContact() {
    // A standing capsule sunk 0.1 into the floor: pushed straight up by 0.1, touching at the point under its core.
    const Capsule sunk{Vec3{0.0f, 0.4f, 0.0f}, Vec3{0.0f, 1.4f, 0.0f}, 0.5f};
    const auto onFloor = contact(sunk, floorA, floorB, floorC);
    assert(onFloor.has_value());
    assert(nearlyEqual(onFloor->depth, 0.1f));
    assertVector(onFloor->normal, Vec3{0.0f, 1.0f, 0.0f});
    assertVector(onFloor->point, Vec3{0.0f, 0.0f, 0.0f});

    // Pressed into the wall from the -x side: pushed back along -x, whichever way the wall triangle is wound.
    const Capsule leaning{Vec3{1.7f, 0.5f, 0.0f}, Vec3{1.7f, 1.5f, 0.0f}, 0.5f};
    const auto onWall = contact(leaning, wallA, wallB, wallC);
    assert(onWall.has_value());
    assert(nearlyEqual(onWall->depth, 0.2f));
    assertVector(onWall->normal, Vec3{-1.0f, 0.0f, 0.0f});
    const auto onWallReversed = contact(leaning, wallC, wallB, wallA);
    assertVector(onWallReversed->normal, Vec3{-1.0f, 0.0f, 0.0f});

    // Exactly touching counts (depth 0); a hair above does not.
    assert(contact(Capsule{Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f, 1.5f, 0.0f}, 0.5f}, floorA, floorB, floorC).has_value());
    assert(!contact(Capsule{Vec3{0.0f, 0.51f, 0.0f}, Vec3{0.0f, 1.5f, 0.0f}, 0.5f}, floorA, floorB, floorC).has_value());

    // Beside the triangle, past its edge: the push comes from the edge, sideways and up.
    const Capsule pastEdge{Vec3{0.0f, 0.3f, 5.3f}, Vec3{0.0f, 1.3f, 5.3f}, 0.5f};
    const auto edge = contact(pastEdge, floorA, floorB, floorC);
    assert(edge.has_value());
    assertVector(edge->normal, Vec3{0.0f, 0.3f, 0.3f}.normalized());
    assert(nearlyEqual(edge->depth, 0.5f - std::sqrt(0.18f)));

    // Each contact also carries the triangle's own face normal, turned to the capsule's side: on the face it is the push
    // direction, past an edge it is not (which is how a ledge's edge is told from a slope's face).
    assertVector(onFloor->faceNormal, Vec3{0.0f, 1.0f, 0.0f});
    assertVector(edge->faceNormal, Vec3{0.0f, 1.0f, 0.0f});
    assertVector(onWallReversed->faceNormal, Vec3{-1.0f, 0.0f, 0.0f});

    // A core that goes right through the floor: no direction from the triangle to the core, so the face normal on the side
    // of the core's middle is used. The depth is how far it must move along that normal to come free: its deepest end is
    // 0.3 behind the plane, plus the radius.
    const Capsule throughFloor{Vec3{0.0f, -0.3f, 0.0f}, Vec3{0.0f, 0.9f, 0.0f}, 0.5f};
    const auto pierced = contact(throughFloor, floorA, floorB, floorC);
    assert(pierced.has_value());
    assertVector(pierced->normal, Vec3{0.0f, 1.0f, 0.0f});
    assert(nearlyEqual(pierced->depth, 0.8f));
    const Capsule mostlyBelow{Vec3{0.0f, -0.9f, 0.0f}, Vec3{0.0f, 0.3f, 0.0f}, 0.5f};
    const auto pushedDown = contact(mostlyBelow, floorA, floorB, floorC);
    assertVector(pushedDown->normal, Vec3{0.0f, -1.0f, 0.0f});
    assert(nearlyEqual(pushedDown->depth, 0.8f));
    assertVector(pushedDown->faceNormal, Vec3{0.0f, -1.0f, 0.0f});

    // A sphere is a capsule whose ends meet.
    const auto ball = contact(Sphere{Vec3{0.0f, 0.3f, 0.0f}, 0.5f}, floorA, floorB, floorC);
    assert(ball.has_value());
    assert(nearlyEqual(ball->depth, 0.2f));
    assertVector(ball->normal, Vec3{0.0f, 1.0f, 0.0f});
}
