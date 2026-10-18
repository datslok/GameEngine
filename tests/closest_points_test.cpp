#include "math/closest_points.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    // A small repeatable generator, so the brute-force comparisons below test the same cases every run.
    class Numbers {
    public:
        float next(float low, float high) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const float unit = static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
            return low + (high - low) * unit;
        }

        Vec3 nextPoint() {
            return Vec3{next(-3.0f, 3.0f), next(-3.0f, 3.0f), next(-3.0f, 3.0f)};
        }

    private:
        std::uint64_t state = 12345;
    };

    // The smallest squared distance found by trying many points along a segment: an upper bound for the exact answer.
    constexpr int samples = 400;

    float sampledSegmentSegment(const Vec3& a1, const Vec3& b1, const Vec3& a2, const Vec3& b2) {
        float best = 1e30f;
        for (int i = 0; i <= samples; ++i) {
            const Vec3 point = a1 + (b1 - a1) * (static_cast<float>(i) / samples);
            best = std::min(best, (closestPointOnSegment(point, a2, b2) - point).lengthSquared());
        }
        return best;
    }

    float sampledSegmentBox(const Vec3& a, const Vec3& b, const Aabb& box) {
        float best = 1e30f;
        for (int i = 0; i <= samples; ++i) {
            const Vec3 point = a + (b - a) * (static_cast<float>(i) / samples);
            best = std::min(best, (closestPointInBox(point, box) - point).lengthSquared());
        }
        return best;
    }

    void testPointQueries() {
        const Vec3 a{0.0f, 0.0f, 0.0f};
        const Vec3 b{4.0f, 0.0f, 0.0f};

        // Beside the middle: straight across. Past either end: that end.
        assertVector(closestPointOnSegment(Vec3{1.0f, 3.0f, 0.0f}, a, b), Vec3{1.0f, 0.0f, 0.0f});
        assertVector(closestPointOnSegment(Vec3{-2.0f, 1.0f, 0.0f}, a, b), a);
        assertVector(closestPointOnSegment(Vec3{9.0f, 0.0f, 1.0f}, a, b), b);

        // A segment of zero length is a point.
        assertVector(closestPointOnSegment(Vec3{1.0f, 1.0f, 1.0f}, b, b), b);

        const Aabb box{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 2.0f, 3.0f}};
        assertVector(closestPointInBox(Vec3{0.5f, 1.0f, 1.5f}, box), Vec3{0.5f, 1.0f, 1.5f});
        assertVector(closestPointInBox(Vec3{-1.0f, 5.0f, 1.0f}, box), Vec3{0.0f, 2.0f, 1.0f});
    }

    void testSegmentPairs() {
        // Crossing at right angles, 2 apart vertically: nearest where they cross.
        const ClosestPoints crossing = closestPointsBetweenSegments(
            Vec3{-1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 2.0f, -1.0f}, Vec3{0.0f, 2.0f, 1.0f});
        assertVector(crossing.onFirst, Vec3{0.0f, 0.0f, 0.0f});
        assertVector(crossing.onSecond, Vec3{0.0f, 2.0f, 0.0f});
        assert(nearlyEqual(crossing.distanceSquared(), 4.0f));

        // The lines cross outside the second segment: clamp to its end, then re-solve on the first.
        const ClosestPoints clamped = closestPointsBetweenSegments(
            Vec3{-1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
            Vec3{0.5f, 0.0f, 2.0f}, Vec3{0.5f, 0.0f, 5.0f});
        assertVector(clamped.onFirst, Vec3{0.5f, 0.0f, 0.0f});
        assertVector(clamped.onSecond, Vec3{0.5f, 0.0f, 2.0f});

        // Parallel and side by side: any nearest pair is 1 apart.
        const ClosestPoints parallel = closestPointsBetweenSegments(
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f},
            Vec3{1.0f, 1.0f, 0.0f}, Vec3{3.0f, 1.0f, 0.0f});
        assert(nearlyEqual(parallel.distanceSquared(), 1.0f));

        // Parallel, end to end in a line: the facing ends.
        const ClosestPoints inLine = closestPointsBetweenSegments(
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
            Vec3{3.0f, 0.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f});
        assert(nearlyEqual(inLine.distanceSquared(), 4.0f));

        // Zero-length segments act as points.
        const ClosestPoints points = closestPointsBetweenSegments(
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f},
            Vec3{0.0f, 3.0f, 0.0f}, Vec3{0.0f, 3.0f, 0.0f});
        assert(nearlyEqual(points.distanceSquared(), 9.0f));

        const ClosestPoints pointAndSegment = closestPointsBetweenSegments(
            Vec3{1.0f, 2.0f, 0.0f}, Vec3{1.0f, 2.0f, 0.0f},
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 0.0f, 0.0f});
        assertVector(pointAndSegment.onSecond, Vec3{1.0f, 0.0f, 0.0f});

        // Many random pairs: never further apart than brute-force sampling finds, and never much nearer.
        Numbers numbers;
        for (int i = 0; i < 200; ++i) {
            const Vec3 a1 = numbers.nextPoint();
            const Vec3 b1 = numbers.nextPoint();
            const Vec3 a2 = numbers.nextPoint();
            const Vec3 b2 = numbers.nextPoint();
            const float exact = closestPointsBetweenSegments(a1, b1, a2, b2).distanceSquared();
            const float sampled = sampledSegmentSegment(a1, b1, a2, b2);
            assert(exact <= sampled + 0.0001f);
            assert(exact >= sampled - 0.01f);
        }
    }

    void testSegmentBox() {
        const Aabb box{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}};

        // Passing straight through the box: they meet.
        const ClosestPoints through = closestPointsSegmentBox(Vec3{-1.0f, 0.5f, 0.5f}, Vec3{2.0f, 0.5f, 0.5f}, box);
        assert(nearlyEqual(through.distanceSquared(), 0.0f));

        // Running alongside the top, 2 above it.
        const ClosestPoints above = closestPointsSegmentBox(Vec3{-1.0f, 3.0f, 0.5f}, Vec3{2.0f, 3.0f, 0.5f}, box);
        assert(nearlyEqual(above.distanceSquared(), 4.0f));

        // Diagonally past a vertical edge: the nearest point is in the middle of the segment, outside on two axes at once.
        const ClosestPoints pastEdge = closestPointsSegmentBox(Vec3{3.0f, 0.0f, 0.5f}, Vec3{0.0f, 3.0f, 0.5f}, box);
        assert(nearlyEqual(pastEdge.distanceSquared(), 0.5f));
        assertVector(pastEdge.onFirst, Vec3{1.5f, 1.5f, 0.5f});
        assertVector(pastEdge.onSecond, Vec3{1.0f, 1.0f, 0.5f});

        // A zero-length segment is a point.
        const ClosestPoints point = closestPointsSegmentBox(Vec3{3.0f, 0.5f, 0.5f}, Vec3{3.0f, 0.5f, 0.5f}, box);
        assert(nearlyEqual(point.distanceSquared(), 4.0f));

        Numbers numbers;
        for (int i = 0; i < 200; ++i) {
            const Vec3 a = numbers.nextPoint();
            const Vec3 b = numbers.nextPoint();
            const Vec3 corner = numbers.nextPoint();
            const Vec3 size{numbers.next(0.0f, 2.0f), numbers.next(0.0f, 2.0f), numbers.next(0.0f, 2.0f)};
            const Aabb randomBox{corner, corner + size};
            const ClosestPoints exact = closestPointsSegmentBox(a, b, randomBox);
            const float sampled = sampledSegmentBox(a, b, randomBox);
            assert(exact.distanceSquared() <= sampled + 0.0001f);
            assert(exact.distanceSquared() >= sampled - 0.01f);
            assertVector(exact.onSecond, closestPointInBox(exact.onSecond, randomBox));
        }
    }
}

void testClosestPoints() {
    testPointQueries();
    testSegmentPairs();
    testSegmentBox();
}
