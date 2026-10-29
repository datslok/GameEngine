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

    // For two segments (closestPointsBetweenSegments), how far along each the points lie, 0 at its start and 1 at its
    // end: onFirst = a1 + (b1 - a1) firstShare. Left at 0 by the other queries.
    float firstShare = 0.0f;
    float secondShare = 0.0f;

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

// The point on the triangle a, b, c nearest to point: on its face, one of its edges or one of its corners.
Vec3 closestPointOnTriangle(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c);

// Which part of a triangle a, b, c its nearest point lies on (edge CA runs from c to a).
enum class TriangleFeature {
    Face,
    EdgeAB,
    EdgeBC,
    EdgeCA,
    CornerA,
    CornerB,
    CornerC,
};

struct TriangleClosest {
    Vec3 point;
    TriangleFeature feature = TriangleFeature::Face;

    // The point's barycentric weights, for a, b and c in x, y and z: point = a x + b y + c z, each 0 to 1, adding up to 1.
    Vec3 weights{1.0f, 0.0f, 0.0f};
};

/*
* closestPointOnTriangle's point and the feature it lies on, for telling a point's side of a closed mesh by that
* feature's normal. A triangle with no area gives its nearest edge.
*/
TriangleClosest closestTriangleFeature(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c);

// The nearest points between the segment from s0 to s1 and the triangle a, b, c (zero distance where the segment pierces it).
ClosestPoints closestPointsSegmentTriangle(const Vec3& s0, const Vec3& s1, const Vec3& a, const Vec3& b, const Vec3& c);
