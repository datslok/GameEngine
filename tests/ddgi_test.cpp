#include "render/gpu/ddgi.h"
#include "math/angles.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return nearlyEqual(actual.x, expected.x, tolerance) && nearlyEqual(actual.y, expected.y, tolerance) &&
               nearlyEqual(actual.z, expected.z, tolerance);
    }

    bool nearlyEqual(const Vec2& actual, const Vec2& expected, float tolerance = 1e-5f) {
        return nearlyEqual(actual.x, expected.x, tolerance) && nearlyEqual(actual.y, expected.y, tolerance);
    }

    // Directions that look random but are the same every run: the Fibonacci points of a large set.
    std::vector<Vec3> manyDirections() {
        std::vector<Vec3> directions;

        for (int i = 0; i < 997; ++i) {
            directions.push_back(sphericalFibonacci(i, 997));
        }

        // The axes and the octahedron's corners and edges, where the mapping folds.
        for (const Vec3& special : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1},
                                    Vec3{1, 1, 0}, Vec3{-1, 1, 0}, Vec3{1, 0, -1}, Vec3{0, -1, -1}, Vec3{1, 1, -1}}) {
            directions.push_back(special.normalized());
        }

        return directions;
    }

    // The square coordinate (-1..1) of a texel centre in a probe's bordered square (inner texels at 1..side).
    Vec2 borderedTexelSquare(TexelCoord bordered, int side) {
        return Vec2{(static_cast<float>(bordered.x - 1) + 0.5f) / static_cast<float>(side) * 2.0f - 1.0f,
                    (static_cast<float>(bordered.y - 1) + 0.5f) / static_cast<float>(side) * 2.0f - 1.0f};
    }

    // A point just outside the square brought back across the fold it crossed: over the edge u = 1, (u, v) is the
    // direction of (2 - u, -v), and likewise for each edge.
    Vec2 foldIntoSquare(Vec2 square) {
        if (square.x > 1.0f) {
            square = Vec2{2.0f - square.x, -square.y};
        } else if (square.x < -1.0f) {
            square = Vec2{-2.0f - square.x, -square.y};
        }

        if (square.y > 1.0f) {
            square = Vec2{-square.x, 2.0f - square.y};
        } else if (square.y < -1.0f) {
            square = Vec2{-square.x, -2.0f - square.y};
        }

        return square;
    }

    // Every direction lands in the square and comes back: the mapping loses nothing.
    void testOctahedralMappingRoundTrips() {
        for (const Vec3& direction : manyDirections()) {
            const Vec2 square = octahedralEncode(direction);
            assert(square.x >= -1.0f && square.x <= 1.0f && square.y >= -1.0f && square.y <= 1.0f);
            assert(nearlyEqual(octahedralDecode(square), direction, 1e-5f));
        }

        // Straight up is the middle; straight down is every corner.
        assert(nearlyEqual(octahedralEncode(Vec3{0, 0, 1}), Vec2{0.0f, 0.0f}));
        for (const Vec2& corner : {Vec2{1, 1}, Vec2{-1, 1}, Vec2{1, -1}, Vec2{-1, -1}}) {
            assert(nearlyEqual(octahedralDecode(corner), Vec3{0, 0, -1}));
        }
    }

    // The square's edges are folds: a point just inside an edge and its mirror image about the edge's middle are
    // neighbours on the sphere, which is what the border copies rely on.
    void testTheSquaresEdgesAreFolds() {
        for (float v : {0.1f, 0.37f, 0.8f, 0.99f}) {
            const float inside = 0.999f;
            const Vec3 near = octahedralDecode(Vec2{inside, v});
            const Vec3 mirrored = octahedralDecode(Vec2{inside, -v});
            assert(near.dot(mirrored) > 0.9999f);

            assert(octahedralDecode(Vec2{v, inside}).dot(octahedralDecode(Vec2{-v, inside})) > 0.9999f);
            assert(octahedralDecode(Vec2{-inside, v}).dot(octahedralDecode(Vec2{-inside, -v})) > 0.9999f);
            assert(octahedralDecode(Vec2{v, -inside}).dot(octahedralDecode(Vec2{-v, -inside})) > 0.9999f);
        }
    }

    // Each texel centre is a unit direction, and that direction maps back into the same texel, at both probe sizes.
    void testTexelCentresAreDirections() {
        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const Vec3 direction = probeTexelDirection(TexelCoord{x, y}, side);
                    assert(nearlyEqual(direction.length(), 1.0f));

                    const Vec2 square = octahedralEncode(direction);
                    assert(static_cast<int>((square.x * 0.5f + 0.5f) * static_cast<float>(side)) == x);
                    assert(static_cast<int>((square.y * 0.5f + 0.5f) * static_cast<float>(side)) == y);
                }
            }
        }
    }

    /*
    * A border texel's centre lies just outside the square; carried back across the fold it crossed, it lands exactly
    * on the centre of the texel it copies, so filtering across the edge blends the right directions. Inner texels copy
    * themselves.
    */
    void testBorderTexelsCopyTheirNeighboursAcrossTheFold() {
        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            for (int y = 0; y < side + 2; ++y) {
                for (int x = 0; x < side + 2; ++x) {
                    const TexelCoord texel{x, y};
                    const TexelCoord source = probeBorderSource(texel, side);
                    const bool inner = x >= 1 && x <= side && y >= 1 && y <= side;

                    assert(source.x >= 1 && source.x <= side && source.y >= 1 && source.y <= side);

                    if (inner) {
                        assert(source.x == x && source.y == y);
                        continue;
                    }

                    const Vec2 folded = foldIntoSquare(borderedTexelSquare(texel, side));
                    assert(nearlyEqual(folded, borderedTexelSquare(source, side), 1e-6f));
                }
            }
        }
    }

    // The update's groups copy the border texels by number: every border texel once, and nothing inside.
    void testBorderTexelsAreNumberedOnce() {
        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            assert(probeBorderTexelCount(side) == 4 * side + 4);
            std::set<std::pair<int, int>> seen;

            for (int number = 0; number < probeBorderTexelCount(side); ++number) {
                const TexelCoord texel = probeBorderTexel(number, side);
                const bool onBorder = texel.x == 0 || texel.y == 0 || texel.x == side + 1 || texel.y == side + 1;
                assert(onBorder);
                assert(texel.x >= 0 && texel.x <= side + 1 && texel.y >= 0 && texel.y <= side + 1);
                assert(seen.insert({texel.x, texel.y}).second);
            }

            // Each group has a thread per inner texel, enough to copy the whole border in one step.
            assert(probeBorderTexelCount(side) <= side * side);
        }
    }

    /*
    * The ray pattern: unit directions, balanced (their mean is close to the centre, so no side of the sphere is
    * favoured) and evenly spread (every direction has a ray within about the angular radius of one ray's share of the
    * sphere).
    */
    void testFibonacciDirectionsCoverTheSphere() {
        for (int count : {16, 64, 256}) {
            Vec3 sum{0.0f, 0.0f, 0.0f};
            std::vector<Vec3> rays;

            for (int i = 0; i < count; ++i) {
                const Vec3 ray = sphericalFibonacci(i, count);
                assert(nearlyEqual(ray.length(), 1.0f));
                sum = sum + ray;
                rays.push_back(ray);
            }

            assert((sum * (1.0f / static_cast<float>(count))).length() < 0.05f);

            // The angular radius of a cap holding one ray's share of the sphere: 4 pi / count = 2 pi (1 - cos r).
            const float shareRadius = std::acos(1.0f - 2.0f / static_cast<float>(count));

            for (const Vec3& direction : manyDirections()) {
                float nearest = -1.0f;

                for (const Vec3& ray : rays) {
                    nearest = std::max(nearest, ray.dot(direction));
                }

                assert(std::acos(std::min(nearest, 1.0f)) < 1.6f * shareRadius);
            }
        }
    }

    /*
    * The frame's rotation: a unit quaternion, the same for the same frame, different from frame to frame, and spread
    * over all orientations (a fixed direction turned by many frames' rotations averages out near zero).
    */
    void testRandomRotationsAreUniformAndRepeatable() {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        const int frames = 4000;

        for (int frame = 0; frame < frames; ++frame) {
            const Quat rotation = randomProbeRotation(static_cast<std::uint64_t>(frame));
            assert(nearlyEqual(rotation.length(), 1.0f));

            const Quat again = randomProbeRotation(static_cast<std::uint64_t>(frame));
            assert(rotation.x == again.x && rotation.y == again.y && rotation.z == again.z && rotation.w == again.w);

            sum = sum + rotation.rotate(Vec3{0.0f, 0.0f, 1.0f});
        }

        assert((sum * (1.0f / static_cast<float>(frames))).length() < 0.05f);

        const Quat first = randomProbeRotation(1);
        const Quat second = randomProbeRotation(2);
        assert(std::abs(first.dot(second)) < 0.999f);
    }

    bool sameRotation(const Quat& a, const Quat& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    /*
    * The shaders turn rays by the rotation's matrix rows: the same as turning by the quaternion. A ray's direction is
    * its point of the long spiral (every probeRayCycle-th from the frame's phase) turned so.
    */
    void testRayDirectionsAreTurnedByTheFramesRotation() {
        const Quat rotation = randomProbeRotation(77);
        const ProbeRotationRows rows = probeRotationRows(rotation);

        for (int phase : {0, 5, probeRayCycle - 1}) {
            for (int ray = 0; ray < 64; ++ray) {
                const Vec3 point = sphericalFibonacci(ray * probeRayCycle + phase, 64 * probeRayCycle);
                const Vec3 turned = rotation.rotate(point);
                assert(nearlyEqual(Vec3{rows.rows[0].dot(point), rows.rows[1].dot(point), rows.rows[2].dot(point)}, turned));
                assert(nearlyEqual(probeRayDirection(ray, 64, phase, rows), turned));
            }
        }
    }

    /*
    * Frames take turns through one spiral: a cycle's frames share its rotation and take the phases in order, the next
    * cycle turns anew, and together a cycle's frames trace every point of the long spiral once, so they cover the
    * sphere as evenly as count x probeRayCycle rays would (every direction within 1.6 of that many rays' share radii),
    * far more evenly than one frame does. The still probes' 64 rays and the fast probes' 256 alike.
    */
    void testFramesTakeTurnsThroughOneSpiral() {
        for (int count : {64, 256}) {
            const std::uint64_t firstFrame = 3 * probeRayCycle;
            const ProbeRayFrame start = probeRayFrame(firstFrame);
            std::vector<Vec3> cycleRays;
            std::vector<Vec3> frameRays;

            for (int step = 0; step < probeRayCycle; ++step) {
                const ProbeRayFrame frame = probeRayFrame(firstFrame + static_cast<std::uint64_t>(step));
                assert(frame.phase == step);
                assert(sameRotation(frame.rotation, start.rotation));

                const ProbeRotationRows rows = probeRotationRows(frame.rotation);

                for (int ray = 0; ray < count; ++ray) {
                    cycleRays.push_back(probeRayDirection(ray, count, frame.phase, rows));

                    if (step == 0) {
                        frameRays.push_back(cycleRays.back());
                    }
                }
            }

            const ProbeRayFrame next = probeRayFrame(firstFrame + probeRayCycle);
            assert(next.phase == 0 && std::abs(next.rotation.dot(start.rotation)) < 0.999f);

            const auto widestGap = [](const std::vector<Vec3>& rays) {
                float widest = 0.0f;

                for (const Vec3& direction : manyDirections()) {
                    float nearest = -1.0f;

                    for (const Vec3& ray : rays) {
                        nearest = std::max(nearest, ray.dot(direction));
                    }

                    widest = std::max(widest, std::acos(std::min(nearest, 1.0f)));
                }

                return widest;
            };

            const float shareRadius = std::acos(1.0f - 2.0f / static_cast<float>(count * probeRayCycle));
            assert(widestGap(cycleRays) < 1.6f * shareRadius);
            assert(widestGap(cycleRays) < 0.5f * widestGap(frameRays));
        }
    }

    // A probe is within reach of a light when the light's range grown by the probes' longest distance reaches it.
    void testProbesWithinReachOfALight() {
        const Vec3 light{2.0f, 1.0f, 0.0f};
        assert(probeNearLight(Vec3{0.0f, 1.0f, 0.0f}, light, 1.5f, 1.0f));
        assert(!probeNearLight(Vec3{0.0f, 1.0f, 0.0f}, light, 0.5f, 1.0f));
        assert(probeNearLight(Vec3{0.0f, 1.0f, 0.0f}, light, 0.5f, 1.6f));
        assert(!probeNearLight(light, light, 0.0f, 0.0f));
        assert(probeNearLight(light, light, 0.1f, 0.0f));
    }

    /*
    * The schedule with the default settings and no fast light near the probes: every probe traces 64 rays (the stride
    * too), frame after frame as probeRayFrame numbers them, with shares kept from the time constants; exactly what the
    * probes did before fast lights, so the golden images do not change.
    */
    void testTheScheduleWithoutFastLights() {
        const GlobalIlluminationSettings settings;
        const float frameSeconds = 1.0f / 120.0f;
        ProbeSchedule schedule;

        for (std::uint64_t number = 0; number < 40; ++number) {
            const ProbeFrame frame = scheduleProbeFrame(schedule, settings, frameSeconds, false, number == 0);
            const ProbeRayFrame expected = probeRayFrame(number);

            assert(frame.raysPerProbe == 64 && frame.fastRaysPerProbe == 64 && probeRayStride(frame) == 64);
            assert(frame.rayPhase == expected.phase && sameRotation(frame.rotation, expected.rotation));
            assert(frame.ballRadius == settings.probeBallRadius);

            if (number == 0) {
                assert(frame.irradianceHysteresis == 0.0f && frame.fastIrradianceHysteresis == 0.0f && frame.distanceHysteresis == 0.0f);
            } else {
                assert(frame.irradianceHysteresis == hysteresisForTimeConstant(frameSeconds, settings.irradianceTimeConstant));
                assert(frame.distanceHysteresis == hysteresisForTimeConstant(frameSeconds, settings.distanceTimeConstant));
                assert(frame.fastIrradianceHysteresis == hysteresisForTimeConstant(frameSeconds, settings.fastIrradianceTimeConstant));
            }
        }

        // Rays outside what a probe may trace are clamped.
        GlobalIlluminationSettings extreme = settings;
        extreme.raysPerProbe = 5000;
        extreme.fastRaysPerProbe = 1;
        const ProbeFrame clamped = scheduleProbeFrame(schedule, extreme, frameSeconds, true, false);
        assert(clamped.raysPerProbe == maxRaysPerProbe && clamped.fastRaysPerProbe == minRaysPerProbe);
    }

    /*
    * With a fast light near the probes: probes near it trace 256 rays (the stride room for 256), the rest 64, on the same
    * cycle; the fast atlas keeps a share from its own time constant, and a slow frame (a hitch of half a second) counts
    * only as 1/30 s for it.
    */
    void testTheScheduleWithFastLights() {
        const GlobalIlluminationSettings settings;
        const float frameSeconds = 1.0f / 240.0f;
        ProbeSchedule schedule;

        for (std::uint64_t number = 0; number < 40; ++number) {
            const ProbeFrame frame = scheduleProbeFrame(schedule, settings, frameSeconds, true, number == 0);
            const ProbeRayFrame expected = probeRayFrame(number);

            assert(frame.raysPerProbe == 64 && frame.fastRaysPerProbe == 256 && probeRayStride(frame) == 256);
            assert(frame.rayPhase == expected.phase && sameRotation(frame.rotation, expected.rotation));
        }

        const ProbeFrame hitch = scheduleProbeFrame(schedule, settings, 0.5f, true, false);
        assert(hitch.fastIrradianceHysteresis == hysteresisForTimeConstant(settings.fastBlendMaxFrameSeconds, settings.fastIrradianceTimeConstant));
        assert(hitch.irradianceHysteresis == hysteresisForTimeConstant(0.5f, settings.irradianceTimeConstant));
    }

    /*
    * The ray budget's ladder: the still rays are divided by a power of two, about the largest that keeps the rays a
    * second at the budget's (frames a second over updates a second, rounded down to a power of two), never below the
    * least a probe may trace; off (0) it is always 1. A step up is taken 5% short of it (114 fps for 2 on a budget of
    * 60, so a jittery 240 settles on 4), a step down a tenth below it, so a frame rate hovering on a step does not flip
    * the count back and forth.
    */
    void testTheRayBudgetsLadder() {
        assert(probeRayDivisor(1, 240.0f, 0.0f, 64) == 1);
        assert(probeRayDivisor(4, 240.0f, 0.0f, 64) == 1);

        assert(probeRayDivisor(1, 60.0f, 60.0f, 64) == 1);
        assert(probeRayDivisor(1, 30.0f, 60.0f, 64) == 1);
        assert(probeRayDivisor(1, 100.0f, 60.0f, 64) == 1);
        assert(probeRayDivisor(1, 120.0f, 60.0f, 64) == 2);
        assert(probeRayDivisor(1, 240.0f, 60.0f, 64) == 4);
        assert(probeRayDivisor(4, 240.0f, 60.0f, 64) == 4);

        // Never under the least a probe may trace (16): 64 rays divide by 4 at most, 32 by 2, 16 not at all.
        assert(probeRayDivisor(1, 1000.0f, 60.0f, 64) == 4);
        assert(probeRayDivisor(1, 240.0f, 60.0f, 32) == 2);
        assert(probeRayDivisor(4, 240.0f, 60.0f, 32) == 2);
        assert(probeRayDivisor(1, 240.0f, 60.0f, 16) == 1);

        // Hysteresis: from 1, 112 fps (1.87 budgets) stays 1 and 115 (1.92) steps up; at 2, 110 (1.83) keeps 2 and 105
        // (1.75) drops to 1; a jittery 235 or 245 is 4 either way.
        assert(probeRayDivisor(1, 112.0f, 60.0f, 64) == 1);
        assert(probeRayDivisor(1, 115.0f, 60.0f, 64) == 2);
        assert(probeRayDivisor(2, 110.0f, 60.0f, 64) == 2);
        assert(probeRayDivisor(2, 105.0f, 60.0f, 64) == 1);
        assert(probeRayDivisor(4, 200.0f, 60.0f, 64) == 2);
        assert(probeRayDivisor(2, 235.0f, 60.0f, 64) == 4 && probeRayDivisor(4, 245.0f, 60.0f, 64) == 4);
    }

    /*
    * The schedule with a budget of 60 updates a second at a steady 240 frames a second: once the smoothed frame time
    * has settled the still probes trace 16 rays (a quarter of 64), probes near a fast light still 256, on the same ray
    * cycle; the shares kept still come from the frame's own time, so the light settles as fast as without the budget.
    */
    void testTheScheduleWithARayBudget() {
        GlobalIlluminationSettings settings;
        settings.probeUpdatesPerSecond = 60.0f;
        const float frameSeconds = 1.0f / 240.0f;
        ProbeSchedule schedule;
        ProbeFrame frame;

        for (std::uint64_t number = 0; number < 480; ++number) {
            frame = scheduleProbeFrame(schedule, settings, frameSeconds, false, number == 0);
            const ProbeRayFrame expected = probeRayFrame(number);
            assert(frame.rayPhase == expected.phase && sameRotation(frame.rotation, expected.rotation));
        }

        assert(frame.raysPerProbe == 16 && frame.fastRaysPerProbe == 16 && probeRayStride(frame) == 16);
        assert(frame.irradianceHysteresis == hysteresisForTimeConstant(frameSeconds, settings.irradianceTimeConstant));
        assert(frame.distanceHysteresis == hysteresisForTimeConstant(frameSeconds, settings.distanceTimeConstant));

        const ProbeFrame withFastLight = scheduleProbeFrame(schedule, settings, frameSeconds, true, false);
        assert(withFastLight.raysPerProbe == 16 && withFastLight.fastRaysPerProbe == 256);

        // At 30 frames a second the budget is not reached: every probe traces all 64, as without it.
        for (int frames = 0; frames < 120; ++frames) {
            frame = scheduleProbeFrame(schedule, settings, 1.0f / 30.0f, false, false);
        }

        assert(frame.raysPerProbe == 64);
    }

    // Irradiance weighs a ray by the cosine between it and the texel (none from behind); distances by a far narrower
    // lobe, the cosine to the 50th power.
    void testUpdateWeights() {
        const Vec3 texel{0.0f, 0.0f, 1.0f};
        const Vec3 tilted = Vec3{std::sin(0.5f), 0.0f, std::cos(0.5f)};

        assert(nearlyEqual(irradianceRayWeight(texel, texel), 1.0f));
        assert(nearlyEqual(irradianceRayWeight(texel, tilted), std::cos(0.5f)));
        assert(irradianceRayWeight(texel, Vec3{1.0f, 0.0f, 0.0f}) == 0.0f);
        assert(irradianceRayWeight(texel, Vec3{0.0f, 0.6f, -0.8f}) == 0.0f);

        assert(nearlyEqual(distanceRayWeight(texel, texel), 1.0f));
        assert(nearlyEqual(distanceRayWeight(texel, tilted), std::pow(std::cos(0.5f), probeDistanceSharpness), 1e-6f));
        assert(distanceRayWeight(texel, Vec3{0.0f, 0.6f, -0.8f}) == 0.0f);
        assert(distanceRayWeight(texel, tilted) < 0.01f);
    }

    // Front-face hits keep their distance, back-face hits (recorded negative) keep a fifth of it, and everything,
    // misses included, stops at the longest distance recorded.
    void testDistancesForTheUpdate() {
        const float longest = 2.6f;

        assert(nearlyEqual(distanceForUpdate(1.2f, longest), 1.2f));
        assert(nearlyEqual(distanceForUpdate(-1.2f, longest), 1.2f * backFaceDistanceShare));
        assert(nearlyEqual(distanceForUpdate(static_cast<float>(DDGI_RAY_REACH), longest), longest));
        assert(nearlyEqual(distanceForUpdate(-50.0f, longest), longest));
        assert(distanceForUpdate(0.0f, longest) == 0.0f);

        assert(nearlyEqual(probeMaxDistance(Vec3{1.0f, 2.0f, 2.0f}), 1.5f * 3.0f));
    }

    /*
    * Hysteresis as a time constant: after tau seconds of frames, 1/e of a step change is left, whether those were 60 or
    * 240 frames a second. A time constant of 0 keeps nothing.
    */
    void testHysteresisFollowsATimeConstant() {
        const float timeConstant = 0.5f;

        for (float framesPerSecond : {30.0f, 60.0f, 240.0f}) {
            const float frameSeconds = 1.0f / framesPerSecond;
            const float hysteresis = hysteresisForTimeConstant(frameSeconds, timeConstant);
            assert(hysteresis > 0.0f && hysteresis < 1.0f);

            float left = 1.0f;
            const int frames = static_cast<int>(std::lround(timeConstant * framesPerSecond));

            for (int frame = 0; frame < frames; ++frame) {
                left = blendProbeValue(Vec3{left, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, hysteresis).x;
            }

            assert(nearlyEqual(left, std::exp(-1.0f), 1e-4f));
        }

        assert(hysteresisForTimeConstant(1.0f / 60.0f, 0.0f) == 0.0f);
        assert(hysteresisForTimeConstant(0.0f, 0.5f) == 1.0f);
    }

    void testBlendingKeepsTheOldShare() {
        assert(nearlyEqual(blendProbeValue(Vec3{2.0f, 4.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 0.75f), Vec3{1.75f, 3.25f, 0.25f}));
        assert(nearlyEqual(blendProbeValue(Vec2{2.0f, 4.0f}, Vec2{1.0f, 1.0f}, 0.75f), Vec2{1.75f, 3.25f}));
    }

    // One texel's cosine-weighted mean of a set of rays, as the update computes it.
    Vec3 irradianceEstimate(const Vec3& texel, const std::vector<Vec3>& directions, const std::vector<Vec3>& radiance) {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        float weights = 0.0f;

        for (std::size_t i = 0; i < directions.size(); ++i) {
            const float weight = irradianceRayWeight(texel, directions[i]);
            sum = sum + radiance[i] * weight;
            weights += weight;
        }

        return sum * (1.0f / weights);
    }

    /*
    * The furnace test: in a uniform sky drawn at B, every texel of every probe stores exactly B, whatever the rotation,
    * because the estimate is normalised by its own weights. That is the ambient light a uniform sky gives, so probe
    * values drop straight into the ambient slot.
    */
    void testAUniformSkyGivesItsOwnValue() {
        const Vec3 sky{0.6f, 0.75f, 1.0f};

        for (std::uint64_t frame : {0u, 5u, 99u}) {
            const ProbeRayFrame rays = probeRayFrame(frame);
            const ProbeRotationRows rows = probeRotationRows(rays.rotation);
            std::vector<Vec3> directions;

            for (int ray = 0; ray < 64; ++ray) {
                directions.push_back(probeRayDirection(ray, 64, rays.phase, rows));
            }

            const std::vector<Vec3> radiance(directions.size(), sky);

            for (int y = 0; y < probeIrradianceTexels; ++y) {
                for (int x = 0; x < probeIrradianceTexels; ++x) {
                    const Vec3 texel = probeTexelDirection(TexelCoord{x, y}, probeIrradianceTexels);
                    assert(nearlyEqual(irradianceEstimate(texel, directions, radiance), sky, 1e-5f));
                }
            }
        }
    }

    /*
    * A sky lit only above the horizon (radiance 1 there, 0 below): a surface tilted theta from straight up receives
    * irradiance pi (1 + cos theta) / 2, so the cosine-weighted mean (irradiance over pi) is (1 + cos theta) / 2. Averaged
    * over frames with turning rays, as hysteresis averages them, each texel converges on that.
    */
    void testAHalfSkyGivesTheCosineLaw() {
        const int frames = 400;
        std::vector<float> sums(static_cast<std::size_t>(probeIrradianceTexels * probeIrradianceTexels), 0.0f);

        for (int frame = 0; frame < frames; ++frame) {
            const ProbeRayFrame rays = probeRayFrame(static_cast<std::uint64_t>(frame));
            const ProbeRotationRows rows = probeRotationRows(rays.rotation);
            std::vector<Vec3> directions;
            std::vector<Vec3> radiance;

            for (int ray = 0; ray < 64; ++ray) {
                const Vec3 direction = probeRayDirection(ray, 64, rays.phase, rows);
                directions.push_back(direction);
                radiance.push_back(direction.y > 0.0f ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.0f, 0.0f, 0.0f});
            }

            for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
                const Vec3 direction = probeTexelDirection(TexelCoord{texel % probeIrradianceTexels, texel / probeIrradianceTexels}, probeIrradianceTexels);
                sums[static_cast<std::size_t>(texel)] += irradianceEstimate(direction, directions, radiance).x;
            }
        }

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const Vec3 direction = probeTexelDirection(TexelCoord{texel % probeIrradianceTexels, texel / probeIrradianceTexels}, probeIrradianceTexels);
            const float expected = (1.0f + direction.y) * 0.5f;
            assert(nearlyEqual(sums[static_cast<std::size_t>(texel)] / static_cast<float>(frames), expected, 0.03f));
        }
    }

    /*
    * A dark room's probe: a small bright patch (a sunlit wall seen through a doorway, 2.0 within 15 degrees of +x, under
    * 2% of the sphere) in near darkness (0.005). Each frame's 64 turned rays catch the patch or miss it, so the texel
    * facing it gets a noisy estimate, which the update blends in at 240 frames a second with the default time constant.
    * The stored value must settle on the estimates' true average, and wander little over a tenth of a second (what the
    * eye sees as crawling). Blending fifth roots with 2021's per-texel speed-ups, as the first version did, failed both
    * (at a 0.5 s time constant it settled at 51% of the truth, since the mean of roots is below the root of the mean, and
    * moved 8.7%, since noise tripped the speed-ups); linear blending settles within 0.1%. Frames taking turns through one
    * spiral (probeRayFrame) must also crawl clearly less than frames each turned at random, as measured on screen.
    */
    struct SparseTexel {
        double truth = 0.0;
        double settled = 0.0;
        double crawl = 0.0;
    };

    SparseTexel sparselyLitTexel(float timeConstant, bool interleaved, int rayCount = 64, float framesPerSecond = 240.0f) {
        const float hysteresis = hysteresisForTimeConstant(1.0f / framesPerSecond, timeConstant);
        const Vec3 towardsPatch{1.0f, 0.0f, 0.0f};
        const float patchCosine = std::cos(radiansFromDegrees(15.0f));

        // The texel whose direction is nearest the patch.
        TexelCoord facing;
        float best = -2.0f;

        for (int y = 0; y < probeIrradianceTexels; ++y) {
            for (int x = 0; x < probeIrradianceTexels; ++x) {
                const float along = probeTexelDirection(TexelCoord{x, y}, probeIrradianceTexels).dot(towardsPatch);

                if (along > best) {
                    best = along;
                    facing = TexelCoord{x, y};
                }
            }
        }

        const Vec3 texel = probeTexelDirection(facing, probeIrradianceTexels);
        const int warmUp = 2000;
        const int frames = 30000;
        const int tenthOfASecond = static_cast<int>(framesPerSecond / 10.0f);
        float stored = 0.0f;
        double estimates = 0.0;
        double storedSum = 0.0;
        double changeSquares = 0.0;
        int changes = 0;
        std::vector<float> history;

        for (int frame = 0; frame < warmUp + frames; ++frame) {
            const std::uint64_t number = static_cast<std::uint64_t>(frame);
            const ProbeRayFrame rays = interleaved ? probeRayFrame(number) : ProbeRayFrame{randomProbeRotation(number), 0};
            const ProbeRotationRows rows = probeRotationRows(rays.rotation);
            std::vector<Vec3> directions;
            std::vector<Vec3> radiance;

            for (int ray = 0; ray < rayCount; ++ray) {
                const Vec3 direction = probeRayDirection(ray, rayCount, rays.phase, rows);
                const float light = direction.dot(towardsPatch) > patchCosine ? 2.0f : 0.005f;
                directions.push_back(direction);
                radiance.push_back(Vec3{light, light, light});
            }

            const float estimate = irradianceEstimate(texel, directions, radiance).x;
            stored = blendProbeValue(Vec3{stored, 0.0f, 0.0f}, Vec3{estimate, 0.0f, 0.0f}, frame == 0 ? 0.0f : hysteresis).x;

            if (frame >= warmUp) {
                estimates += estimate;
                storedSum += stored;
                history.push_back(stored);

                if (history.size() > static_cast<std::size_t>(tenthOfASecond)) {
                    const double change = stored - history[history.size() - 1 - static_cast<std::size_t>(tenthOfASecond)];
                    changeSquares += change * change;
                    ++changes;
                }
            }
        }

        const double truth = estimates / frames;
        return SparseTexel{truth, storedSum / frames, std::sqrt(changeSquares / changes) / truth};
    }

    void testASparselyLitTexelSettlesOnItsTrueMean() {
        const GlobalIlluminationSettings settings;
        const SparseTexel random = sparselyLitTexel(0.5f, false);
        const SparseTexel interleaved = sparselyLitTexel(0.5f, true);
        const SparseTexel chosen = sparselyLitTexel(settings.irradianceTimeConstant, true);

        // A fast light's part: its 256 rays and its 0.05 s.
        const SparseTexel fast = sparselyLitTexel(settings.fastIrradianceTimeConstant, true, settings.fastRaysPerProbe);

        for (const SparseTexel* texel : {&random, &interleaved, &chosen, &fast}) {
            assert(std::abs(texel->settled / texel->truth - 1.0) < 0.02);
        }

        // Measured: 2.4% for random rotations, 1.3% interleaved, 0.33% interleaved at the 2 s default.
        assert(interleaved.crawl < 0.7 * random.crawl);
        assert(chosen.crawl < 0.01);

        // A ray budget of 60 updates a second at 240 fps (16 rays a frame) crawls like 64 rays at 60 fps, since noise goes
        // with rays a second: measured 0.95% against 0.96%, and 0.33% at 240 fps with all 64.
        const SparseTexel budget = sparselyLitTexel(settings.irradianceTimeConstant, true, 16, 240.0f);
        const SparseTexel sixty = sparselyLitTexel(settings.irradianceTimeConstant, true, 64, 60.0f);
        assert(std::abs(budget.settled / budget.truth - 1.0) < 0.02);
        assert(budget.crawl < 1.1 * sixty.crawl && budget.crawl > 2.0 * chosen.crawl);
    }

    // Probes are numbered x first, then y, then z, and sit on the grid's corners from the origin.
    void testProbePositionsFollowTheGrid() {
        const ProbeVolume volume{Vec3{-2.0f, 0.5f, 3.0f}, Vec3{1.0f, 0.5f, 2.0f}, ProbeCounts{4, 3, 5}};

        assert(probeCount(volume.counts) == 60);
        assert(probeIndex(volume.counts, 0, 0, 0) == 0);
        assert(probeIndex(volume.counts, 1, 0, 0) == 1);
        assert(probeIndex(volume.counts, 0, 1, 0) == 4);
        assert(probeIndex(volume.counts, 0, 0, 1) == 12);
        assert(probeIndex(volume.counts, 3, 2, 4) == 59);

        assert(nearlyEqual(probePosition(volume, 0), Vec3{-2.0f, 0.5f, 3.0f}));
        assert(nearlyEqual(probePosition(volume, probeIndex(volume.counts, 3, 2, 4)), Vec3{1.0f, 1.5f, 11.0f}));
        assert(nearlyEqual(probePosition(volume, probeIndex(volume.counts, 1, 2, 3)), Vec3{-1.0f, 1.5f, 9.0f}));
    }

    /*
    * The atlases: every probe's bordered square inside the image and none overlapping, each row of squares one layer of
    * the grid; and the place to sample for a texel's own direction is that texel's centre in that probe's square.
    */
    void testProbeAtlasLayout() {
        const ProbeCounts counts{4, 3, 5};

        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            const int bordered = side + 2;
            const ProbeAtlasSize size = probeAtlasSize(counts, side);
            assert(size.width == 12 * bordered && size.height == 5 * bordered);

            std::set<std::pair<int, int>> origins;

            for (int probe = 0; probe < probeCount(counts); ++probe) {
                const TexelCoord origin = probeTileOrigin(counts, probe, side);
                assert(origin.x % bordered == 0 && origin.y % bordered == 0);
                assert(origin.x + bordered <= size.width && origin.y + bordered <= size.height);
                assert(origins.insert({origin.x, origin.y}).second);
                assert(origin.y / bordered == probe / 12);

                for (const TexelCoord texel : {TexelCoord{0, 0}, TexelCoord{side - 1, 2}, TexelCoord{3, side - 1}}) {
                    const Vec2 uv = probeAtlasUv(counts, probe, probeTexelDirection(texel, side), side);
                    const Vec2 centre{(static_cast<float>(origin.x + 1 + texel.x) + 0.5f) / static_cast<float>(size.width),
                                      (static_cast<float>(origin.y + 1 + texel.y) + 0.5f) / static_cast<float>(size.height)};
                    assert(nearlyEqual(uv, centre, 1e-5f));
                }
            }
        }
    }

    bool rejects(const ProbeVolume& volume) {
        try {
            validateProbeVolume(volume);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // Volumes the renderer cannot hold are refused: no probes along an axis, spacing that is not positive, more probes
    // than one dispatch runs, or squares that do not fit in the largest image.
    void testImpossibleVolumesAreRejected() {
        assert(!rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{16, 8, 16}}));
        assert(!rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}}));
        assert(rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{0, 8, 16}}));
        assert(rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 0.0f, 1.0f}, ProbeCounts{4, 4, 4}}));
        assert(rejects(ProbeVolume{Vec3{}, Vec3{-1.0f, 1.0f, 1.0f}, ProbeCounts{4, 4, 4}}));
        assert(rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{64, 32, 64}}));
        assert(rejects(ProbeVolume{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1000, 1, 2}}));
    }

    // The push constants carry the volume and the frame's values where the shaders read them.
    void testProbeVolumePacking() {
        const ProbeVolume volume{Vec3{-2.0f, 0.5f, 3.0f}, Vec3{1.0f, 0.5f, 2.0f}, ProbeCounts{4, 3, 5}};
        ProbeFrame frame;
        frame.raysPerProbe = 96;
        frame.fastRaysPerProbe = 200;
        frame.rotation = randomProbeRotation(3);
        frame.rayPhase = 7;
        frame.sky = Vec3{0.2f, 0.3f, 0.4f};
        frame.irradianceHysteresis = 0.97f;
        frame.fastIrradianceHysteresis = 0.5f;
        frame.distanceHysteresis = 0.98f;
        frame.ballRadius = 0.15f;

        const GpuProbeVolume packed = packProbeVolume(volume, frame);
        const ProbeRotationRows rows = probeRotationRows(frame.rotation);

        assert(packed.origin[0] == -2.0f && packed.origin[1] == 0.5f && packed.origin[2] == 3.0f && packed.origin[3] == 0.5f);
        assert(packed.spacing[0] == 1.0f && packed.spacing[1] == 0.5f && packed.spacing[2] == 2.0f);
        assert(nearlyEqual(packed.spacing[3], probeMaxDistance(volume.spacing)));
        assert(packed.counts[0] == 4 && packed.counts[1] == 3 && packed.counts[2] == 5 && packed.counts[3] == 200);
        assert(probeRayStride(frame) == 200);

        for (int row = 0; row < 3; ++row) {
            assert(nearlyEqual(Vec3{packed.rotation[row][0], packed.rotation[row][1], packed.rotation[row][2]}, rows.rows[row]));
        }

        assert(packed.rotation[0][3] == 96.0f && packed.rotation[1][3] == 200.0f);
        assert(packed.sky[0] == 0.2f && packed.sky[1] == 0.3f && packed.sky[2] == 0.4f);
        assert(packed.hysteresis[0] == 0.97f && packed.hysteresis[1] == 0.98f && packed.hysteresis[2] == 0.15f && packed.hysteresis[3] == 7.0f);
    }
}

void testDdgi() {
    testOctahedralMappingRoundTrips();
    testTheSquaresEdgesAreFolds();
    testTexelCentresAreDirections();
    testBorderTexelsCopyTheirNeighboursAcrossTheFold();
    testBorderTexelsAreNumberedOnce();
    testFibonacciDirectionsCoverTheSphere();
    testRandomRotationsAreUniformAndRepeatable();
    testRayDirectionsAreTurnedByTheFramesRotation();
    testFramesTakeTurnsThroughOneSpiral();
    testProbesWithinReachOfALight();
    testTheScheduleWithoutFastLights();
    testTheScheduleWithFastLights();
    testTheRayBudgetsLadder();
    testTheScheduleWithARayBudget();
    testUpdateWeights();
    testDistancesForTheUpdate();
    testHysteresisFollowsATimeConstant();
    testBlendingKeepsTheOldShare();
    testAUniformSkyGivesItsOwnValue();
    testAHalfSkyGivesTheCosineLaw();
    testASparselyLitTexelSettlesOnItsTrueMean();
    testProbePositionsFollowTheGrid();
    testProbeAtlasLayout();
    testImpossibleVolumesAreRejected();
    testProbeVolumePacking();
}
