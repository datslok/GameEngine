#include "physics/overlap.h"

#include "math/closest_points.h"

#include <variant>

namespace {
    // Two rounded shapes touch when their cores are no further apart than the radii added together.
    bool withinReach(float distanceSquared, float reach) {
        return distanceSquared <= reach * reach;
    }
}

/*
* Two balls touch when their centres are no further apart than the radii added together.
*/
bool overlaps(const Sphere& first, const Sphere& second) {
    return withinReach((second.centre - first.centre).lengthSquared(), first.radius + second.radius);
}

/*
* The capsule is its segment grown by its radius, so measure from the sphere's centre to the nearest point of the segment.
*/
bool overlaps(const Sphere& sphere, const Capsule& capsule) {
    const Vec3 nearest = closestPointOnSegment(sphere.centre, capsule.a, capsule.b);
    return withinReach((sphere.centre - nearest).lengthSquared(), sphere.radius + capsule.radius);
}

/*
* The nearest point of the box must lie inside the sphere. Testing the sphere's own bounding box instead would wrongly
* report hits just off the box's corners and edges.
*/
bool overlaps(const Sphere& sphere, const Aabb& box) {
    const Vec3 nearest = closestPointInBox(sphere.centre, box);
    return withinReach((sphere.centre - nearest).lengthSquared(), sphere.radius);
}

/*
* Two grown segments: compare the nearest points of the segments with both radii.
*/
bool overlaps(const Capsule& first, const Capsule& second) {
    const ClosestPoints nearest = closestPointsBetweenSegments(first.a, first.b, second.a, second.b);
    return withinReach(nearest.distanceSquared(), first.radius + second.radius);
}

/*
* The box has no radius, so the segment must come within the capsule's radius of it.
*/
bool overlaps(const Capsule& capsule, const Aabb& box) {
    const ClosestPoints nearest = closestPointsSegmentBox(capsule.a, capsule.b, box);
    return withinReach(nearest.distanceSquared(), capsule.radius);
}

/*
* Boxes that never rotate overlap exactly when their ranges overlap on all three axes.
*/
bool overlaps(const Aabb& first, const Aabb& second) {
    return first.minimum.x <= second.maximum.x && second.minimum.x <= first.maximum.x
        && first.minimum.y <= second.maximum.y && second.minimum.y <= first.maximum.y
        && first.minimum.z <= second.maximum.z && second.minimum.z <= first.maximum.z;
}

// The reversed orders, so callers never have to remember which way round a pair goes.
bool overlaps(const Capsule& capsule, const Sphere& sphere) {
    return overlaps(sphere, capsule);
}

bool overlaps(const Aabb& box, const Sphere& sphere) {
    return overlaps(sphere, box);
}

bool overlaps(const Aabb& box, const Capsule& capsule) {
    return overlaps(capsule, box);
}

/*
* std::visit calls the lambda with both shapes' real types, and overload resolution then picks the matching test above.
*/
bool overlaps(const Shape& first, const Shape& second) {
    return std::visit([](const auto& a, const auto& b) { return overlaps(a, b); }, first, second);
}
