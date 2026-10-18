#pragma once

#include "math/aabb.h"
#include "math/vec3.h"

/*
* Closest-point queries between points, line segments and boxes. Collision between rounded shapes reduces to these:
* a sphere is a point grown by its radius and a capsule is a segment grown by its radius, so two of them touch when
* the distance between their cores is at most the sum of the radii. Raycasts and contact normals build on them too.
*/

// The pair of points, one on each of two things, that are nearest each other.
struct ClosestPoints {
    Vec3 onFirst;
    Vec3 onSecond;

    float distanceSquared() const;
};

// The point on the segment from a to b nearest to point. A segment with a == b is just that point.
Vec3 closestPointOnSegment(const Vec3& point, const Vec3& a, const Vec3& b);

// The point inside or on the box nearest to point: the point itself when it is inside.
Vec3 closestPointInBox(const Vec3& point, const Aabb& box);

// The nearest points between the segments from a1 to b1 and from a2 to b2 (one of the pairs, when the segments are parallel).
ClosestPoints closestPointsBetweenSegments(const Vec3& a1, const Vec3& b1, const Vec3& a2, const Vec3& b2);

// The nearest points between the segment from a to b and the box (onSecond is in the box; zero distance when they cross).
ClosestPoints closestPointsSegmentBox(const Vec3& a, const Vec3& b, const Aabb& box);
