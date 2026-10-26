#include "math/closest_points.h"
#include "math/closest_points_shared.h"

#include <algorithm>
#include <array>

namespace {
    // Below this squared length a segment is treated as a single point, so nothing divides by (nearly) zero
    // (math/closest_points_shared.h, which cloth.glsl reads too).
    constexpr float degenerateLengthSquared = static_cast<float>(CLOSEST_POINTS_DEGENERATE_LENGTH_SQUARED);

    float clamp01(float value) {
        return std::clamp(value, 0.0f, 1.0f);
    }
}

/*
* Squared, because comparing against a squared radius needs no square root.
*/
float ClosestPoints::distanceSquared() const {
    return (onSecond - onFirst).lengthSquared();
}

/*
* Project the point onto the segment's line and clamp to the ends: the projection is the nearest point on the infinite
* line, and outside 0..1 the nearest point on the segment is the end it ran past.
*/
Vec3 closestPointOnSegment(const Vec3& point, const Vec3& a, const Vec3& b) {
    const Vec3 direction = b - a;
    const float lengthSquared = direction.lengthSquared();

    if (lengthSquared <= degenerateLengthSquared) {
        return a;
    }

    const float t = clamp01((point - a).dot(direction) / lengthSquared);
    return a + direction * t;
}

/*
* Clamp each coordinate into the box's range. The axes are independent for an axis-aligned box, so clamping each one
* separately gives the nearest point.
*/
Vec3 closestPointInBox(const Vec3& point, const Aabb& box) {
    return Vec3{
        std::clamp(point.x, box.minimum.x, box.maximum.x),
        std::clamp(point.y, box.minimum.y, box.maximum.y),
        std::clamp(point.z, box.minimum.z, box.maximum.z)
    };
}

/*
* The squared distance between points s and t along the two segments is a quadratic in (s, t); its minimum over the unit
* square is found by solving for the infinite lines' nearest points, then clamping one parameter and re-solving the other,
* which is enough because the quadratic is convex. Follows Ericson, "Real-Time Collision Detection", section 5.1.9.
*/
ClosestPoints closestPointsBetweenSegments(const Vec3& a1, const Vec3& b1, const Vec3& a2, const Vec3& b2) {
    const Vec3 direction1 = b1 - a1;
    const Vec3 direction2 = b2 - a2;
    const Vec3 between = a1 - a2;

    const float lengthSquared1 = direction1.lengthSquared();
    const float lengthSquared2 = direction2.lengthSquared();
    const float f = direction2.dot(between);

    float s = 0.0f;
    float t = 0.0f;

    if (lengthSquared1 <= degenerateLengthSquared && lengthSquared2 <= degenerateLengthSquared) {
        return ClosestPoints{a1, a2};
    }

    if (lengthSquared1 <= degenerateLengthSquared) {
        t = clamp01(f / lengthSquared2);
    } else {
        const float c = direction1.dot(between);

        if (lengthSquared2 <= degenerateLengthSquared) {
            s = clamp01(-c / lengthSquared1);
        } else {
            const float b = direction1.dot(direction2);
            const float denominator = lengthSquared1 * lengthSquared2 - b * b;

            // Parallel segments have a whole range of nearest pairs; start from s = 0 and the clamping below picks one.
            if (denominator > degenerateLengthSquared * lengthSquared1 * lengthSquared2) {
                s = clamp01((b * f - c * lengthSquared2) / denominator);
            }

            t = (b * s + f) / lengthSquared2;

            if (t < 0.0f) {
                t = 0.0f;
                s = clamp01(-c / lengthSquared1);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clamp01((b - c) / lengthSquared1);
            }
        }
    }

    return ClosestPoints{a1 + direction1 * s, a2 + direction2 * t};
}

/*
* The squared distance from a point moving along the segment to the box is convex, and between the places where the point
* crosses one of the box's six planes it is a plain quadratic: on each axis the point is either below the box, above it or
* within its range, and only the axes outside count. So cut the segment at those crossings and minimise each quadratic
* piece exactly; the best piece wins. No iteration, and it also handles segments that pass through the box (distance 0).
*/
ClosestPoints closestPointsSegmentBox(const Vec3& a, const Vec3& b, const Aabb& box) {
    const Vec3 direction = b - a;

    // 0, up to two plane crossings per axis, and 1, kept in order: each crossing is slid into its place as it is found
    // (an insertion sort, the simplest sort for a handful of numbers).
    std::array<float, 8> cuts{};
    int cutCount = 0;
    cuts[cutCount++] = 0.0f;

    for (int index = 0; index < 3; ++index) {
        const float step = direction[index];

        if (step == 0.0f) {
            continue;
        }

        for (const float plane : {box.minimum[index], box.maximum[index]}) {
            const float t = (plane - a[index]) / step;

            if (t > 0.0f && t < 1.0f) {
                int place = cutCount++;
                while (place > 0 && cuts[place - 1] > t) {
                    cuts[place] = cuts[place - 1];
                    --place;
                }
                cuts[place] = t;
            }
        }
    }

    cuts[cutCount++] = 1.0f;

    ClosestPoints best{a, closestPointInBox(a, box)};
    float bestDistanceSquared = best.distanceSquared();

    for (int piece = 0; piece + 1 < cutCount; ++piece) {
        const float start = cuts[piece];
        const float end = cuts[piece + 1];
        const Vec3 middle = a + direction * ((start + end) * 0.5f);

        // Within this piece each axis stays on one side of the box, so the squared distance is
        // sum over outside axes of (a + direction * t - plane)^2. Its derivative is zero at t = -numerator / denominator.
        float numerator = 0.0f;
        float denominator = 0.0f;

        for (int index = 0; index < 3; ++index) {
            const float position = middle[index];
            const float minimum = box.minimum[index];
            const float maximum = box.maximum[index];

            if (position < minimum || position > maximum) {
                const float plane = position < minimum ? minimum : maximum;
                const float step = direction[index];
                numerator += step * (a[index] - plane);
                denominator += step * step;
            }
        }

        // With no outside axis that moves, the distance is the same all along the piece, so its middle will do.
        const float t = denominator > 0.0f ? std::clamp(-numerator / denominator, start, end) : (start + end) * 0.5f;
        const Vec3 onSegment = a + direction * t;
        const ClosestPoints candidate{onSegment, closestPointInBox(onSegment, box)};
        const float candidateDistanceSquared = candidate.distanceSquared();

        if (candidateDistanceSquared < bestDistanceSquared) {
            best = candidate;
            bestDistanceSquared = candidateDistanceSquared;
        }
    }

    return best;
}

/*
* The plane around a triangle splits into seven regions by which feature is nearest: the face (straight above or below it),
* three edges and three corners. A few dot products of the point's offset with the edges are, in effect, the signs of its
* barycentric coordinates, and they say which region it is in; only that region's case is then worked out. Corners are
* checked first because they need the fewest products. Follows Ericson, "Real-Time Collision Detection", section 5.1.5.
*/
TriangleClosest closestTriangleFeature(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;

    const Vec3 ap = point - a;
    const float d1 = ab.dot(ap);
    const float d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return {a, TriangleFeature::CornerA};
    }

    const Vec3 bp = point - b;
    const float d3 = ab.dot(bp);
    const float d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return {b, TriangleFeature::CornerB};
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return {a + ab * (d1 / (d1 - d3)), TriangleFeature::EdgeAB};
    }

    const Vec3 cp = point - c;
    const float d5 = ab.dot(cp);
    const float d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return {c, TriangleFeature::CornerC};
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return {a + ac * (d2 / (d2 - d6)), TriangleFeature::EdgeCA};
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return {b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))), TriangleFeature::EdgeBC};
    }

    // Inside the face region. The three products are proportional to the barycentric weights; a triangle with no area
    // makes them all zero, and then its nearest edge is the answer.
    const float total = va + vb + vc;
    if (total <= 0.0f) {
        TriangleClosest best{closestPointOnSegment(point, a, b), TriangleFeature::EdgeAB};
        for (const TriangleClosest& candidate : {TriangleClosest{closestPointOnSegment(point, b, c), TriangleFeature::EdgeBC},
                                                 TriangleClosest{closestPointOnSegment(point, c, a), TriangleFeature::EdgeCA}}) {
            if ((candidate.point - point).lengthSquared() < (best.point - point).lengthSquared()) {
                best = candidate;
            }
        }
        return best;
    }
    return {a + ab * (vb / total) + ac * (vc / total), TriangleFeature::Face};
}

Vec3 closestPointOnTriangle(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c) {
    return closestTriangleFeature(point, a, b, c).point;
}

/*
* Either the segment pierces the triangle (distance zero, where it crosses the plane inside the triangle), or the nearest
* pair has an end of the segment or an edge of the triangle in it: if both nearest points were inside (the segment's middle
* and the triangle's face), sliding along the segment would bring them closer, unless the segment is parallel to the face,
* in which case its ends are just as near. So the answer is the best of: each end against the triangle, and the segment
* against each edge.
*/
ClosestPoints closestPointsSegmentTriangle(const Vec3& s0, const Vec3& s1, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 normal = (b - a).cross(c - a);
    const float side0 = normal.dot(s0 - a);
    const float side1 = normal.dot(s1 - a);

    if ((side0 <= 0.0f && side1 >= 0.0f) || (side0 >= 0.0f && side1 <= 0.0f)) {
        if (side0 != side1) {
            const Vec3 crossing = s0 + (s1 - s0) * (side0 / (side0 - side1));
            const Vec3 onTriangle = closestPointOnTriangle(crossing, a, b, c);
            if ((onTriangle - crossing).lengthSquared() <= degenerateLengthSquared * normal.length()) {
                return ClosestPoints{crossing, crossing};
            }
        }
    }

    ClosestPoints best{s0, closestPointOnTriangle(s0, a, b, c)};
    const auto consider = [&best](const ClosestPoints& candidate) {
        if (candidate.distanceSquared() < best.distanceSquared()) {
            best = candidate;
        }
    };

    consider(ClosestPoints{s1, closestPointOnTriangle(s1, a, b, c)});
    consider(closestPointsBetweenSegments(s0, s1, a, b));
    consider(closestPointsBetweenSegments(s0, s1, b, c));
    consider(closestPointsBetweenSegments(s0, s1, c, a));
    return best;
}
