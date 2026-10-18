#include "physics/triangle_contact.h"

#include "math/closest_points.h"

#include <algorithm>
#include <cmath>

namespace {
    // Below this distance the core counts as touching the triangle itself, and the direction between them is too unsure
    // to push along.
    constexpr float coreTouchDistance = 1e-6f;
}

/*
* A capsule is its core segment grown by the radius, so it presses into the triangle by radius minus the distance from
* the core to the triangle, along the line between their nearest points. The face normal is only needed when that line
* has no length (the core reaches the triangle): then the capsule is pushed out on the side its middle is on, far enough
* that the core's end on the other side comes back to the plane, plus the radius.
*/
std::optional<Contact> contact(const Capsule& capsule, const Vec3& a, const Vec3& b, const Vec3& c) {
    const ClosestPoints nearest = closestPointsSegmentTriangle(capsule.a, capsule.b, a, b, c);
    const float distanceSquared = nearest.distanceSquared();
    if (distanceSquared > capsule.radius * capsule.radius) {
        return std::nullopt;
    }

    const float distance = std::sqrt(distanceSquared);
    if (distance > coreTouchDistance) {
        return Contact{nearest.onSecond, (nearest.onFirst - nearest.onSecond) / distance, capsule.radius - distance};
    }

    const Vec3 face = (b - a).cross(c - a);
    if (face.lengthSquared() == 0.0f) {
        return std::nullopt;
    }

    Vec3 normal = face.normalized();
    const Vec3 middle = (capsule.a + capsule.b) * 0.5f;
    if (normal.dot(middle - a) < 0.0f) {
        normal = normal * -1.0f;
    }

    // How far each end of the core is behind the plane (negative if in front); the deeper one decides.
    const float behind = std::max({0.0f, -normal.dot(capsule.a - a), -normal.dot(capsule.b - a)});
    return Contact{nearest.onSecond, normal, behind + capsule.radius};
}

/*
* A sphere is a capsule whose two ends are its centre.
*/
std::optional<Contact> contact(const Sphere& sphere, const Vec3& a, const Vec3& b, const Vec3& c) {
    return contact(Capsule{sphere.centre, sphere.centre, sphere.radius}, a, b, c);
}
