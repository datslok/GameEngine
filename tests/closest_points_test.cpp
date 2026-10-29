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

namespace {
    // The smallest squared distance from a point to many points spread over the triangle (as weights of its corners).
    float sampledPointTriangle(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c) {
        constexpr int steps = 60;
        float best = 1e30f;
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; i + j <= steps; ++j) {
                const float u = static_cast<float>(i) / steps;
                const float v = static_cast<float>(j) / steps;
                const Vec3 onTriangle = a + (b - a) * u + (c - a) * v;
                best = std::min(best, (onTriangle - point).lengthSquared());
            }
        }
        return best;
    }

    void testPointTriangle() {
        // A right triangle in the z = 0 plane.
        const Vec3 a{0.0f, 0.0f, 0.0f};
        const Vec3 b{4.0f, 0.0f, 0.0f};
        const Vec3 c{0.0f, 4.0f, 0.0f};

        // Above the face: straight down onto it.
        assertVector(closestPointOnTriangle(Vec3{1.0f, 1.0f, 3.0f}, a, b, c), Vec3{1.0f, 1.0f, 0.0f});
        // The three corner regions.
        assertVector(closestPointOnTriangle(Vec3{-1.0f, -1.0f, 2.0f}, a, b, c), a);
        assertVector(closestPointOnTriangle(Vec3{6.0f, -1.0f, 0.0f}, a, b, c), b);
        assertVector(closestPointOnTriangle(Vec3{-1.0f, 6.0f, -2.0f}, a, b, c), c);
        // The three edge regions: below edge ab, left of edge ac, and beyond the slanted edge bc.
        assertVector(closestPointOnTriangle(Vec3{2.0f, -3.0f, 1.0f}, a, b, c), Vec3{2.0f, 0.0f, 0.0f});
        assertVector(closestPointOnTriangle(Vec3{-2.0f, 1.5f, 0.0f}, a, b, c), Vec3{0.0f, 1.5f, 0.0f});
        assertVector(closestPointOnTriangle(Vec3{3.0f, 3.0f, 1.0f}, a, b, c), Vec3{2.0f, 2.0f, 0.0f});

        // The same seven regions name the feature the closest point lies on (for inside-outside tests by its normal).
        const auto feature = [&](const Vec3& point) {
            return closestTriangleFeature(point, a, b, c).feature;
        };
        assert(feature(Vec3{1.0f, 1.0f, 3.0f}) == TriangleFeature::Face);
        assert(feature(Vec3{-1.0f, -1.0f, 2.0f}) == TriangleFeature::CornerA);
        assert(feature(Vec3{6.0f, -1.0f, 0.0f}) == TriangleFeature::CornerB);
        assert(feature(Vec3{-1.0f, 6.0f, -2.0f}) == TriangleFeature::CornerC);
        assert(feature(Vec3{2.0f, -3.0f, 1.0f}) == TriangleFeature::EdgeAB);
        assert(feature(Vec3{3.0f, 3.0f, 1.0f}) == TriangleFeature::EdgeBC);
        assert(feature(Vec3{-2.0f, 1.5f, 0.0f}) == TriangleFeature::EdgeCA);
        assertVector(closestTriangleFeature(Vec3{3.0f, 3.0f, 1.0f}, a, b, c).point, Vec3{2.0f, 2.0f, 0.0f});

        // Random points and triangles: never further than the sampled nearest point, never much nearer.
        Numbers numbers;
        for (int i = 0; i < 200; ++i) {
            const Vec3 p = numbers.nextPoint();
            const Vec3 t0 = numbers.nextPoint();
            const Vec3 t1 = numbers.nextPoint();
            const Vec3 t2 = numbers.nextPoint();
            const float exact = (closestPointOnTriangle(p, t0, t1, t2) - p).lengthSquared();
            const float sampled = sampledPointTriangle(p, t0, t1, t2);
            assert(exact <= sampled + 0.0001f);
            assert(exact >= sampled - 0.05f);
        }
    }

    void testSegmentTriangle() {
        const Vec3 a{0.0f, 0.0f, 0.0f};
        const Vec3 b{4.0f, 0.0f, 0.0f};
        const Vec3 c{0.0f, 4.0f, 0.0f};

        // Parallel above the face, 2 up.
        const ClosestPoints above = closestPointsSegmentTriangle(Vec3{0.5f, 0.5f, 2.0f}, Vec3{1.5f, 1.0f, 2.0f}, a, b, c);
        assert(nearlyEqual(above.distanceSquared(), 4.0f));

        // Passing beside the slanted edge, outside the triangle: nearest to that edge.
        const ClosestPoints beside = closestPointsSegmentTriangle(Vec3{3.0f, 3.0f, -1.0f}, Vec3{3.0f, 3.0f, 1.0f}, a, b, c);
        assert(nearlyEqual(beside.distanceSquared(), 2.0f));
        assertVector(beside.onSecond, Vec3{2.0f, 2.0f, 0.0f});

        // Piercing the face: they meet where the segment crosses it.
        const ClosestPoints pierce = closestPointsSegmentTriangle(Vec3{1.0f, 1.0f, -1.0f}, Vec3{1.0f, 1.0f, 3.0f}, a, b, c);
        assert(nearlyEqual(pierce.distanceSquared(), 0.0f));
        assertVector(pierce.onFirst, Vec3{1.0f, 1.0f, 0.0f});

        // Random cases against sampling along the segment.
        Numbers numbers;
        for (int i = 0; i < 100; ++i) {
            const Vec3 s0 = numbers.nextPoint();
            const Vec3 s1 = numbers.nextPoint();
            const Vec3 t0 = numbers.nextPoint();
            const Vec3 t1 = numbers.nextPoint();
            const Vec3 t2 = numbers.nextPoint();
            const float exact = closestPointsSegmentTriangle(s0, s1, t0, t1, t2).distanceSquared();
            float sampled = 1e30f;
            for (int step = 0; step <= 200; ++step) {
                const Vec3 point = s0 + (s1 - s0) * (static_cast<float>(step) / 200.0f);
                sampled = std::min(sampled, (closestPointOnTriangle(point, t0, t1, t2) - point).lengthSquared());
            }
            assert(exact <= sampled + 0.0001f);
            assert(exact >= sampled - 0.02f);
        }
    }
}

namespace {
    /*
    * The weights and shares self-collision spreads its pushes by: a triangle's nearest point rebuilt from its corners by
    * the barycentric weights (each 0 to 1, adding to 1) for every feature it can land on, a flat triangle's too; and two
    * segments' nearest points rebuilt from their ends by the shares.
    */
    void testWeightsAndShares() {
        Numbers numbers;
        for (int trial = 0; trial < 2000; ++trial) {
            const Vec3 point = numbers.nextPoint();
            const Vec3 a = numbers.nextPoint();
            const Vec3 b = numbers.nextPoint();
            // Every tenth triangle flat: its third corner on its first edge.
            const Vec3 c = trial % 10 == 0 ? a + (b - a) * 0.3f : numbers.nextPoint();

            const TriangleClosest closest = closestTriangleFeature(point, a, b, c);
            const Vec3& weights = closest.weights;
            assert(weights.x >= 0.0f && weights.y >= 0.0f && weights.z >= 0.0f);
            assert(nearlyEqual(weights.x + weights.y + weights.z, 1.0f));
            assertVector(a * weights.x + b * weights.y + c * weights.z, closest.point);

            const ClosestPoints segments = closestPointsBetweenSegments(point, a, b, c);
            assert(segments.firstShare >= 0.0f && segments.firstShare <= 1.0f);
            assert(segments.secondShare >= 0.0f && segments.secondShare <= 1.0f);
            assertVector(point + (a - point) * segments.firstShare, segments.onFirst);
            assertVector(b + (c - b) * segments.secondShare, segments.onSecond);
        }

        const TriangleClosest corner = closestTriangleFeature(Vec3{-1.0f, -1.0f, 0.0f}, Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        assert(corner.feature == TriangleFeature::CornerA);
        assertVector(corner.weights, Vec3{1.0f, 0.0f, 0.0f});
    }
}

void testClosestPoints() {
    testWeightsAndShares();
    testPointQueries();
    testSegmentPairs();
    testSegmentBox();
    testPointTriangle();
    testSegmentTriangle();
}
