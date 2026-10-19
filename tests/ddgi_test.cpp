#include "render/gpu/ddgi.h"

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

    // The shaders turn rays by the rotation's matrix rows: the same as turning by the quaternion. A ray's direction is
    // its Fibonacci point turned so.
    void testRayDirectionsAreTurnedByTheFramesRotation() {
        const Quat rotation = randomProbeRotation(77);
        const ProbeRotationRows rows = probeRotationRows(rotation);

        for (int ray = 0; ray < 64; ++ray) {
            const Vec3 point = sphericalFibonacci(ray, 64);
            const Vec3 turned = rotation.rotate(point);
            assert(nearlyEqual(Vec3{rows.rows[0].dot(point), rows.rows[1].dot(point), rows.rows[2].dot(point)}, turned));
            assert(nearlyEqual(probeRayDirection(ray, 64, rows), turned));
        }
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

    // Stored as the fifth root and back: 0 and 1 stay put, 32 is stored as 2, and the round trip loses nothing.
    void testIrradianceEncoding() {
        assert(nearlyEqual(encodeIrradiance(Vec3{0.0f, 1.0f, 32.0f}), Vec3{0.0f, 1.0f, 2.0f}));

        for (const Vec3& value : {Vec3{0.001f, 0.25f, 3.0f}, Vec3{40.0f, 0.0f, 7.5f}}) {
            assert(nearlyEqual(decodeIrradiance(encodeIrradiance(value)), value, value.length() * 1e-5f));
        }
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

    /*
    * Big changes speed a texel up: past the change threshold its hysteresis drops by 0.15 (never below 0), past the
    * reset threshold it takes the new value outright. The largest change of any channel counts, whichever way it goes:
    * green falling a lot while red rises a little is caught.
    */
    void testBigChangesLowerATexelsHysteresis() {
        const float base = 0.97f;
        const Vec3 old{1.0f, 1.0f, 1.0f};

        assert(irradianceTexelHysteresis(base, old, Vec3{1.1f, 0.9f, 1.2f}) == base);
        assert(nearlyEqual(irradianceTexelHysteresis(base, old, Vec3{1.3f, 1.0f, 1.0f}), base - irradianceChangeHysteresisDrop));
        assert(nearlyEqual(irradianceTexelHysteresis(base, old, Vec3{1.05f, 0.6f, 1.0f}), base - irradianceChangeHysteresisDrop));
        assert(irradianceTexelHysteresis(base, old, Vec3{1.0f, 1.0f, 0.1f}) == 0.0f);
        assert(irradianceTexelHysteresis(0.1f, old, Vec3{1.4f, 1.0f, 1.0f}) == 0.0f);

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
            const ProbeRotationRows rows = probeRotationRows(randomProbeRotation(frame));
            std::vector<Vec3> directions;

            for (int ray = 0; ray < 64; ++ray) {
                directions.push_back(probeRayDirection(ray, 64, rows));
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
            const ProbeRotationRows rows = probeRotationRows(randomProbeRotation(static_cast<std::uint64_t>(frame)));
            std::vector<Vec3> directions;
            std::vector<Vec3> radiance;

            for (int ray = 0; ray < 64; ++ray) {
                const Vec3 direction = probeRayDirection(ray, 64, rows);
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
        frame.rotation = randomProbeRotation(3);
        frame.sky = Vec3{0.2f, 0.3f, 0.4f};
        frame.irradianceHysteresis = 0.97f;
        frame.distanceHysteresis = 0.98f;
        frame.ballRadius = 0.15f;

        const GpuProbeVolume packed = packProbeVolume(volume, frame);
        const ProbeRotationRows rows = probeRotationRows(frame.rotation);

        assert(packed.origin[0] == -2.0f && packed.origin[1] == 0.5f && packed.origin[2] == 3.0f);
        assert(packed.spacing[0] == 1.0f && packed.spacing[1] == 0.5f && packed.spacing[2] == 2.0f);
        assert(nearlyEqual(packed.spacing[3], probeMaxDistance(volume.spacing)));
        assert(packed.counts[0] == 4 && packed.counts[1] == 3 && packed.counts[2] == 5 && packed.counts[3] == 96);

        for (int row = 0; row < 3; ++row) {
            assert(nearlyEqual(Vec3{packed.rotation[row][0], packed.rotation[row][1], packed.rotation[row][2]}, rows.rows[row]));
        }

        assert(packed.sky[0] == 0.2f && packed.sky[1] == 0.3f && packed.sky[2] == 0.4f);
        assert(packed.hysteresis[0] == 0.97f && packed.hysteresis[1] == 0.98f && packed.hysteresis[2] == 0.15f);
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
    testUpdateWeights();
    testDistancesForTheUpdate();
    testIrradianceEncoding();
    testHysteresisFollowsATimeConstant();
    testBigChangesLowerATexelsHysteresis();
    testAUniformSkyGivesItsOwnValue();
    testAHalfSkyGivesTheCosineLaw();
    testProbePositionsFollowTheGrid();
    testProbeAtlasLayout();
    testImpossibleVolumesAreRejected();
    testProbeVolumePacking();
}
