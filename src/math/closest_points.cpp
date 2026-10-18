#include "math/closest_points.h"

#include <algorithm>
#include <array>

namespace {
    // Below this squared length a segment is treated as a single point, so nothing divides by (nearly) zero.
    constexpr float degenerateLengthSquared = 1e-12f;

    float clamp01(float value) {
        return std::clamp(value, 0.0f, 1.0f);
    }

    float axis(const Vec3& vector, int index) {
        return index == 0 ? vector.x : (index == 1 ? vector.y : vector.z);
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

    // 0, 1 and up to two plane crossings per axis.
    std::array<float, 8> cuts{};
    int cutCount = 0;
    cuts[cutCount++] = 0.0f;
    cuts[cutCount++] = 1.0f;

    for (int index = 0; index < 3; ++index) {
        const float step = axis(direction, index);

        if (step == 0.0f) {
            continue;
        }

        for (const float plane : {axis(box.minimum, index), axis(box.maximum, index)}) {
            const float t = (plane - axis(a, index)) / step;

            if (t > 0.0f && t < 1.0f) {
                cuts[cutCount++] = t;
            }
        }
    }

    std::sort(cuts.begin(), cuts.begin() + cutCount);

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
            const float position = axis(middle, index);
            const float minimum = axis(box.minimum, index);
            const float maximum = axis(box.maximum, index);

            if (position < minimum || position > maximum) {
                const float plane = position < minimum ? minimum : maximum;
                const float step = axis(direction, index);
                numerator += step * (axis(a, index) - plane);
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
