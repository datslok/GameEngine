#include "render/gpu/brdf.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/light_falloff.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return nearlyEqual(actual.x, expected.x, tolerance) && nearlyEqual(actual.y, expected.y, tolerance) &&
               nearlyEqual(actual.z, expected.z, tolerance);
    }

    // Bit for bit the same.
    bool identical(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    const ProbeVolume unitVolume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{4, 4, 4}};

    // Moments of a probe that sees nothing nearby: every surface far beyond any point it is asked about.
    constexpr float farAway = 100.0f;

    Vec2 openMoments() {
        return Vec2{farAway, farAway * farAway};
    }

    // Every probe storing the same irradiance (and none from fast lights), seeing open space all round.
    ProbeLookup uniformProbes(const Vec3& irradiance) {
        return ProbeLookup{[irradiance](int, const Vec3&) { return irradiance; }, [](int, const Vec3&) { return openMoments(); }, nullptr};
    }

    void testSelfShadowBiasLeansTowardsTheViewer() {
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const Vec3 side{1.0f, 0.0f, 0.0f};
        const Vec3 spacing{2.0f, 1.0f, 3.0f};

        // Scaled by the smallest spacing (1 here).
        const Vec3 straight = probeSelfShadowBias(up, up, spacing, 0.3f);
        assert(nearlyEqual(straight, up * (biasSpacingShare * 0.3f)));

        // Mostly towards the viewer, a little along the normal.
        const Vec3 grazing = probeSelfShadowBias(up, side, spacing, 1.0f);
        assert(nearlyEqual(grazing, (up * biasNormalShare + side * biasViewShare) * biasSpacingShare));

        assert(nearlyEqual(probeSelfShadowBias(up, side, spacing, 0.0f), Vec3{0.0f, 0.0f, 0.0f}));
    }

    void testTheCellAroundAPoint() {
        const ProbeVolume volume{Vec3{-2.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.5f, 2.0f}, ProbeCounts{3, 4, 2}};

        const ProbeCell inside = probeCell(volume, Vec3{-0.75f, 1.6f, 1.0f});
        assert(inside.base.x == 1 && inside.base.y == 1 && inside.base.z == 0);
        assert(nearlyEqual(inside.alpha, Vec3{0.25f, 0.2f, 0.5f}));

        // Corners count x first, then y, then z.
        const ProbeGridCoord last = probeCellCorner(inside, volume.counts, 7);
        assert(last.x == 2 && last.y == 2 && last.z == 1);
        const ProbeGridCoord yOnly = probeCellCorner(inside, volume.counts, 2);
        assert(yOnly.x == 1 && yOnly.y == 2 && yOnly.z == 0);

        // Outside the grid the cell is clamped: before the first probe alpha is 0, past the last both corners are the last.
        const ProbeCell outside = probeCell(volume, Vec3{-5.0f, 9.0f, 3.0f});
        assert(outside.base.x == 0 && outside.base.y == 3 && outside.base.z == 1);
        assert(nearlyEqual(outside.alpha.x, 0.0f));
        const ProbeGridCoord clamped = probeCellCorner(outside, volume.counts, 7);
        assert(clamped.x == 1 && clamped.y == 3 && clamped.z == 1);

        // A single probe along an axis: every corner is that probe.
        const ProbeVolume flat{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{2, 1, 2}};
        const ProbeCell layer = probeCell(flat, Vec3{0.5f, 0.3f, 0.5f});
        assert(probeCellCorner(layer, flat.counts, 2).y == 0);
    }

    /*
    * Trilinear weights share out exactly 1, and reproduce any linear function of position from its values at the
    * corners: what makes the blend smooth.
    */
    void testTrilinearWeightsInterpolateLinearly() {
        const Vec3 alpha{0.3f, 0.6f, 0.85f};
        float total = 0.0f;
        float interpolated = 0.0f;

        const auto linear = [](float x, float y, float z) { return 2.0f + 3.0f * x - 1.5f * y + 0.5f * z; };

        for (int corner = 0; corner < 8; ++corner) {
            const float weight = probeTrilinearWeight(alpha, corner);
            assert(weight > 0.0f);
            total += weight;
            interpolated += weight * linear(static_cast<float>(corner & 1), static_cast<float>((corner >> 1) & 1),
                                            static_cast<float>((corner >> 2) & 1));
        }

        assert(nearlyEqual(total, 1.0f));
        assert(nearlyEqual(interpolated, linear(alpha.x, alpha.y, alpha.z), 1e-4f));

        // At a corner the far corners keep the smallest factor rather than nothing.
        assert(nearlyEqual(probeTrilinearWeight(Vec3{0.0f, 0.0f, 0.0f}, 7), minTrilinearFactor * minTrilinearFactor * minTrilinearFactor, 1e-12f));
        assert(nearlyEqual(probeTrilinearWeight(Vec3{0.0f, 0.0f, 0.0f}, 0), 1.0f));
    }

    void testProbesBehindTheSurfaceCountLess() {
        const Vec3 normal{0.0f, 1.0f, 0.0f};
        assert(nearlyEqual(probeBackFaceWeight(normal, Vec3{0.0f, 3.0f, 0.0f}), 1.0f + probeWrapFloor));
        assert(nearlyEqual(probeBackFaceWeight(normal, Vec3{2.0f, 0.0f, 0.0f}), 0.25f + probeWrapFloor));
        assert(nearlyEqual(probeBackFaceWeight(normal, Vec3{0.0f, -0.5f, 0.0f}), probeWrapFloor));

        // A probe at the point itself sees it fully.
        assert(nearlyEqual(probeBackFaceWeight(normal, Vec3{0.0f, 0.0f, 0.0f}), 1.0f + probeWrapFloor));
    }

    void testChebyshevVisibility() {
        const float mean = 2.0f;
        const float variance = 0.04f;
        const Vec2 moments{mean, variance + mean * mean};

        // Nearer than the mean: seen.
        assert(probeVisibility(0.5f, moments) == 1.0f);
        assert(probeVisibility(mean, moments) == 1.0f);

        // Beyond it, the cubed bound, falling steadily, never below the floor.
        const float beyond = mean + 0.1f;
        const float bound = variance / (variance + 0.01f);
        assert(nearlyEqual(probeVisibility(beyond, moments), bound * bound * bound, 1e-4f));

        float previous = 1.0f;

        for (float distance = mean; distance < mean + 5.0f; distance += 0.05f) {
            const float visibility = probeVisibility(distance, moments);
            assert(visibility <= previous + 1e-6f);
            assert(visibility >= minProbeVisibility);
            previous = visibility;
        }

        assert(nearlyEqual(probeVisibility(mean + 5.0f, moments), minProbeVisibility));

        // No spread at all (a flat wall seen head-on): anything behind it is hidden, down to the floor.
        assert(nearlyEqual(probeVisibility(1.01f, Vec2{1.0f, 1.0f}), minProbeVisibility));

        // Rounding can make the mean square a hair below the mean's square; that is no spread, not a negative one.
        assert(probeVisibility(1.5f, Vec2{1.0f, 0.9999f}) >= minProbeVisibility);
    }

    void testSmallWeightsAreCrushed() {
        assert(crushProbeWeight(0.7f) == 0.7f);
        assert(nearlyEqual(crushProbeWeight(probeCrushThreshold), probeCrushThreshold));
        assert(nearlyEqual(crushProbeWeight(probeCrushThreshold - 1e-4f), probeCrushThreshold, 1e-3f));
        assert(nearlyEqual(crushProbeWeight(0.1f), 0.001f / (probeCrushThreshold * probeCrushThreshold)));

        // Never zero, so a point every probe is hidden from still divides by something.
        assert(crushProbeWeight(0.0f) > 0.0f);
        assert(nearlyEqual(crushProbeWeight(0.0f), std::pow(smallestProbeWeight, 3.0f) / (probeCrushThreshold * probeCrushThreshold), 1e-20f));
    }

    void testACornersWholeWeight() {
        const Vec3 normal{0.0f, 1.0f, 0.0f};
        const Vec3 toProbe{0.0f, 1.0f, 0.0f};
        const Vec2 moments{2.0f, 4.04f};

        // Seen and in front: the crush does nothing, the trilinear weight multiplies.
        assert(nearlyEqual(probeCornerWeight(normal, toProbe, 1.0f, moments, 0.3f), (1.0f + probeWrapFloor) * 0.3f));

        // Hidden and behind: crushed before the trilinear weight.
        const float hidden = probeWrapFloor * probeVisibility(4.0f, moments);
        assert(nearlyEqual(probeCornerWeight(normal, toProbe * -1.0f, 4.0f, moments, 0.5f), crushProbeWeight(hidden) * 0.5f, 1e-9f));
    }

    // A cascade's weight along each axis: full to reach - fade from the centre, half way through the fade, nothing past it.
    void testTheCascadeWeightFadesRoundItsCentre() {
        const Vec3 centre{1.0f, 2.0f, 3.0f};
        const Vec3 reach{4.0f, 2.0f, 4.0f};
        const Vec3 fade{1.0f, 0.5f, 1.0f};
        assert(probeCascadeWeight(centre, reach, fade, centre) == 1.0f);
        assert(probeCascadeWeight(centre, reach, fade, Vec3{-2.0f, 3.5f, 0.0f}) == 1.0f);
        assert(nearlyEqual(probeCascadeWeight(centre, reach, fade, Vec3{4.5f, 2.0f, 3.0f}), 0.5f));
        assert(nearlyEqual(probeCascadeWeight(centre, reach, fade, Vec3{4.5f, 0.25f, 3.0f}), 0.25f));
        assert(probeCascadeWeight(centre, reach, fade, Vec3{1.0f, 2.0f, 7.0f}) == 0.0f);
    }

    void testTheBlendAveragesSquareRoots() {
        ProbeBlend same;
        addProbeSample(same, Vec3{0.8f, 2.0f, 0.1f}, 0.3f);
        addProbeSample(same, Vec3{0.8f, 2.0f, 0.1f}, 2.0f);
        assert(nearlyEqual(finishProbeBlend(same), Vec3{0.8f, 2.0f, 0.1f}, 1e-4f));

        // Equal weights on 0 and 4: the mean of the square roots (1), squared.
        ProbeBlend two;
        addProbeSample(two, Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
        addProbeSample(two, Vec3{4.0f, 4.0f, 4.0f}, 1.0f);
        assert(nearlyEqual(finishProbeBlend(two), Vec3{1.0f, 1.0f, 1.0f}, 1e-4f));
    }

    // The furnace: probes that all store B give B everywhere, whatever the surface's place, normal or viewer.
    void testUniformProbesGiveTheirOwnValue() {
        const Vec3 sky{0.6f, 0.75f, 1.0f};
        const ProbeLookup lookup = uniformProbes(sky);

        for (int i = 0; i < 50; ++i) {
            const float t = static_cast<float>(i) * 0.37f;
            const Vec3 point{std::fmod(t, 3.0f), std::fmod(t * 1.7f, 3.0f), std::fmod(t * 2.3f, 3.0f)};
            const Vec3 normal = sphericalFibonacci(i, 50);
            const Vec3 toViewer = (normal + sphericalFibonacci(49 - i, 50)).normalized();
            const Vec3 reflected = sphericalFibonacci((i * 7) % 50, 50);
            const ProbeLights light = sampleProbes(unitVolume, 0.3f, point, normal, normal, reflected, toViewer, lookup);
            assert(nearlyEqual(light.still.diffuse, sky, 1e-3f));
            assert(nearlyEqual(light.still.specular, sky, 1e-3f));

            // No fast lights: nothing at all in their part.
            assert(identical(light.fast.diffuse, Vec3{}) && identical(light.fast.specular, Vec3{}));
        }
    }

    /*
    * The fast lights' atlas is blended on its own with the same corner weights: each part is exactly what sampling that
    * atlas alone gives, so with the fast atlas empty the still part is the whole light, as before fast lights.
    */
    void testFastLightIsBlendedApart() {
        const auto stillLight = [](int probe, const Vec3& direction) {
            return Vec3{0.2f + 0.1f * static_cast<float>(probe % 4), 0.5f, direction.y > 0.0f ? 1.0f : 0.3f};
        };
        const auto fastLight = [](int probe, const Vec3&) { return Vec3{0.05f * static_cast<float>(probe % 7), 0.0f, 0.4f}; };
        const auto moments = [](int probe, const Vec3&) { return Vec2{0.8f + 0.05f * static_cast<float>(probe % 5), 1.0f}; };

        const ProbeLookup both{stillLight, moments, fastLight};
        const ProbeLookup stillAlone{stillLight, moments, nullptr};
        const ProbeLookup fastAlone{fastLight, moments, nullptr};

        const Vec3 point{1.3f, 1.6f, 0.7f};
        const Vec3 normal = Vec3{0.3f, 1.0f, -0.2f}.normalized();
        const Vec3 reflected = Vec3{-0.5f, 0.6f, 0.4f}.normalized();
        const Vec3 toViewer = Vec3{0.1f, 0.7f, 0.7f}.normalized();

        const ProbeLights split = sampleProbes(unitVolume, 0.3f, point, normal, normal, reflected, toViewer, both);
        const ProbeLights still = sampleProbes(unitVolume, 0.3f, point, normal, normal, reflected, toViewer, stillAlone);
        const ProbeLights fast = sampleProbes(unitVolume, 0.3f, point, normal, normal, reflected, toViewer, fastAlone);

        assert(identical(split.still.diffuse, still.still.diffuse) && identical(split.still.specular, still.still.specular));
        assert(identical(split.fast.diffuse, fast.still.diffuse) && identical(split.fast.specular, fast.still.specular));
    }

    /*
    * Only traced probes light a surface, each where relocation moved it (render/gpu/probe_relocation.h): with the far
    * side of the cell asleep, the light is the near side's alone; moving the traced probes changes their weights; with
    * no traced probe round the point, the flat ambient light, and nothing from fast lights.
    */
    void testOnlyTracedProbesLight() {
        const Vec3 near{1.0f, 0.5f, 0.25f};
        const auto byColumn = [near](int probe, const Vec3&) { return probe % 4 <= 1 ? near : Vec3{0.1f, 0.9f, 0.6f}; };
        const auto open = [](int, const Vec3&) { return openMoments(); };
        const Vec3 point{1.4f, 1.5f, 1.6f};
        const Vec3 normal{0.0f, 1.0f, 0.0f};
        const Vec3 ambient{0.3f, 0.3f, 0.35f};

        ProbeLookup farAsleep{byColumn, open, byColumn};
        farAsleep.ambient = ambient;
        farAsleep.state = [](int probe) {
            return GpuProbeState{{0.0f, 0.0f, 0.0f}, static_cast<std::int32_t>(probe % 4 <= 1 ? ProbeState::Vigilant : ProbeState::Sleeping)};
        };
        const ProbeLights nearOnly = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, farAsleep);
        assert(nearlyEqual(nearOnly.still.diffuse, near, 1e-4f));

        ProbeLookup moved{byColumn, open, nullptr};
        moved.state = [](int probe) { return GpuProbeState{{probe % 4 <= 1 ? 0.3f : 0.0f, 0.0f, 0.0f}, static_cast<std::int32_t>(ProbeState::Awake)}; };
        const ProbeLights inPlace = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, ProbeLookup{byColumn, open, nullptr});
        const ProbeLights shifted = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, moved);
        assert(!nearlyEqual(inPlace.still.diffuse, shifted.still.diffuse, 1e-4f));

        ProbeLookup allOff = farAsleep;
        allOff.state = [](int) { return GpuProbeState{{0.0f, 0.0f, 0.0f}, static_cast<std::int32_t>(ProbeState::Off)}; };
        const ProbeLights none = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, allOff);
        assert(identical(none.still.diffuse, ambient) && identical(none.still.specular, ambient));
        assert(identical(none.fast.diffuse, Vec3{}) && identical(none.fast.specular, Vec3{}));
    }

    // The specular stand-in is the same blend read in the mirror direction: probes brighter from above give an upward
    // reflection more light than a downward one, with the same weights.
    void testSpecularReadsTheMirrorDirection() {
        const ProbeLookup skyAbove{[](int, const Vec3& direction) { return direction.y > 0.0f ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.1f, 0.1f, 0.1f}; },
                                   [](int, const Vec3&) { return openMoments(); }, nullptr};
        const Vec3 point{1.5f, 1.5f, 1.5f};
        const Vec3 side{1.0f, 0.0f, 0.0f};
        const Vec3 up{0.0f, 1.0f, 0.0f};

        const ProbeLight upwards = sampleProbes(unitVolume, 0.3f, point, side, side, up, side, skyAbove).still;
        const ProbeLight downwards = sampleProbes(unitVolume, 0.3f, point, side, side, up * -1.0f, side, skyAbove).still;
        assert(nearlyEqual(upwards.specular, Vec3{1.0f, 1.0f, 1.0f}, 1e-3f));
        assert(nearlyEqual(downwards.specular, Vec3{0.1f, 0.1f, 0.1f}, 1e-3f));
        assert(nearlyEqual(upwards.diffuse, downwards.diffuse));
    }

    /*
    * Leak prevention: a wall at x = 0.5 between the lit probes (x = 0) and the dark ones (x = 1 and beyond). A point on
    * the wall's lit face, seen from the lit side, gets the lit side's light: the dark probes see the wall closer than the
    * point, so Chebyshev's test hides them, and the crush takes what is left. Without the distances (every probe seeing
    * open space) the dark probes would pull it well down.
    */
    void testAWallKeepsTheDarkSidesProbesOut() {
        const float wall = 0.5f;
        const Vec3 lit{1.0f, 1.0f, 1.0f};
        const Vec3 dark{0.0f, 0.0f, 0.0f};
        const auto probeX = [](int probe) { return static_cast<float>(probe % unitVolume.counts.x); };

        const ProbeLookup walled{
            [&](int probe, const Vec3&) { return probeX(probe) < wall ? lit : dark; },
            [&](int probe, const Vec3& direction) {
                const float x = probeX(probe);
                const bool facesWall = (x < wall) != (direction.x < 0.0f) && std::abs(direction.x) > 1e-4f;

                if (!facesWall) {
                    return openMoments();
                }

                const float distance = std::abs(wall - x) / std::abs(direction.x);
                return Vec2{distance, distance * distance + 1e-4f};
            },
            nullptr};

        const ProbeLookup open{walled.irradiance, [](int, const Vec3&) { return openMoments(); }, nullptr};

        const Vec3 point{wall, 1.5f, 1.5f};
        const Vec3 normal{-1.0f, 0.0f, 0.0f};
        const Vec3 withWall = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, walled).still.diffuse;
        const Vec3 withoutWall = sampleProbes(unitVolume, 0.3f, point, normal, normal, normal, normal, open).still.diffuse;

        assert(withWall.x > 0.99f);
        assert(withoutWall.x < 0.9f);
    }

    void testTheBrightestLightsAreKept() {
        BrightestLights kept;

        for (const auto& [light, strength] : {std::pair{0, 0.5f}, std::pair{1, 2.0f}, std::pair{2, 0.0f}, std::pair{3, 1.0f},
                                              std::pair{4, 0.2f}, std::pair{5, 1.0f}, std::pair{6, 3.0f}}) {
            keepBrightestLight(kept, 3, light, strength);
        }

        // Brightest first; of the two at 1.0 the earlier stays.
        assert(kept.count == 3);
        assert(kept.lights[0] == 6 && kept.lights[1] == 1 && kept.lights[2] == 3);
        assert(kept.strengths[0] == 3.0f && kept.strengths[2] == 1.0f);

        // Room for more than there are: everything that gives light, in order.
        BrightestLights roomy;
        keepBrightestLight(roomy, maxLightsPerHit, 0, 0.0f);
        keepBrightestLight(roomy, maxLightsPerHit, 1, 0.4f);
        keepBrightestLight(roomy, maxLightsPerHit, 2, 0.9f);
        assert(roomy.count == 2 && roomy.lights[0] == 2 && roomy.lights[1] == 1);

        // No room at all: nothing kept.
        BrightestLights none;
        keepBrightestLight(none, 0, 0, 5.0f);
        assert(none.count == 0);
    }

    void testLightAtAProbeRaysHit() {
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const Vec3 radiance{2.0f, 1.0f, 0.5f};

        // A lamp 2 m up and 1 m across: radiance times falloff times the cosine.
        const Vec3 offset{1.0f, 2.0f, 0.0f};
        const float distance = offset.length();
        const Vec3 expected = radiance * (distanceFalloff(distance, 10.0f, 0.1f) * (2.0f / distance));
        assert(nearlyEqual(pointLightAtHit(offset, 10.0f, radiance, 0.1f, up), expected));

        // Behind the surface, out of range, or at the surface itself: nothing.
        assert(nearlyEqual(pointLightAtHit(Vec3{0.0f, -1.0f, 0.0f}, 10.0f, radiance, 0.1f, up), Vec3{0.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(pointLightAtHit(offset, 2.0f, radiance, 0.1f, up), Vec3{0.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(pointLightAtHit(Vec3{0.0f, 0.0f, 0.0f}, 2.0f, radiance, 0.1f, up), Vec3{0.0f, 0.0f, 0.0f}));

        // A spot pointing straight down at the surface, and the same one turned away.
        const Vec3 down{0.0f, -1.0f, 0.0f};
        const Vec3 above{0.0f, 3.0f, 0.0f};
        const float cosOuter = std::cos(0.5f);
        const float cosInner = std::cos(0.3f);
        const Vec3 lit = spotLightAtHit(above, 10.0f, radiance, 1.0f, down, cosOuter, cosInner, up);
        assert(nearlyEqual(lit, radiance * distanceFalloff(3.0f, 10.0f, 1.0f)));
        assert(nearlyEqual(spotLightAtHit(above, 10.0f, radiance, 1.0f, up, cosOuter, cosInner, up), Vec3{0.0f, 0.0f, 0.0f}));

        assert(nearlyEqual(hitLightStrength(Vec3{1.0f, 1.0f, 1.0f}), 1.0f));
        assert(hitLightStrength(Vec3{0.0f, 1.0f, 0.0f}) > hitLightStrength(Vec3{1.0f, 0.0f, 0.0f}));
    }

    /*
    * The lights a probe ray's hit may need: those whose range reaches the probes' box grown by the longest distance the
    * probes record, numbered as in the frame's buffers (positions there are measured from the camera), points first,
    * each marked fast or not by its slot's flag (none past the flags' end).
    */
    void testTheLightsNearTheProbesAreListed() {
        const Vec3 cameraOrigin{10.0f, 0.0f, 0.0f};
        const float reach = probeMaxDistance(unitVolume.spacing);

        const auto point = [&](const Vec3& world, float range) {
            GpuPointLight light{};
            const Vec3 relative = world - cameraOrigin;
            light.positionRange[0] = relative.x;
            light.positionRange[1] = relative.y;
            light.positionRange[2] = relative.z;
            light.positionRange[3] = range;
            return light;
        };

        GpuLights lights;
        lights.points.push_back(point(Vec3{1.5f, 1.5f, 1.5f}, 1.0f));               // inside
        lights.points.push_back(point(Vec3{3.0f + reach + 2.0f, 1.0f, 1.0f}, 1.0f)); // beyond, its range short of the reach
        lights.points.push_back(point(Vec3{3.0f + reach + 2.0f, 1.0f, 1.0f}, 2.5f)); // the same, its range reaching in

        GpuSpotLight spot{};
        spot.positionRange[0] = -cameraOrigin.x - 50.0f;
        spot.positionRange[3] = 5.0f;
        lights.spots.push_back(spot);
        spot.positionRange[0] = -cameraOrigin.x;
        lights.spots.push_back(spot);

        lights.fastPoints = {false, true, true};
        lights.fastSpots = {true, true};

        const ProbeHitLights listed = probeHitLights(lights, cameraOrigin, unitVolume);
        assert(listed.points == 2 && listed.spots == 1 && listed.fast == 2);
        assert(listed.lights.size() == 3);
        assert(listed.lights[0].light == 0 && listed.lights[0].fast == 0);
        assert(listed.lights[1].light == 2 && listed.lights[1].fast == 1);
        assert(listed.lights[2].light == 1 && listed.lights[2].fast == 1);

        // With fast lights not kept apart (GlobalIlluminationSettings::fastLights off), the same lights, none fast.
        const ProbeHitLights merged = probeHitLights(lights, cameraOrigin, unitVolume, false);
        assert(merged.points == 2 && merged.spots == 1 && merged.fast == 0 && merged.lights.size() == 3);

        for (std::size_t i = 0; i < merged.lights.size(); ++i) {
            assert(merged.lights[i].light == listed.lights[i].light && merged.lights[i].fast == 0);
        }

        assert(GlobalIlluminationSettings{}.fastLights);

        // Without flags, nothing is fast.
        lights.fastPoints.clear();
        lights.fastSpots.clear();
        const ProbeHitLights unflagged = probeHitLights(lights, cameraOrigin, unitVolume);
        assert(unflagged.fast == 0 && unflagged.lights[1].fast == 0 && unflagged.lights[2].fast == 0);
    }

    void testProbeSamplingIsPacked() {
        ProbeSampling sampling;
        sampling.probes = 960;
        sampling.cascades = 3;
        sampling.selfShadowBias = 0.4f;
        sampling.bounceEnergy = 0.9f;
        sampling.lightsPerHit = 3;
        sampling.hitPoints = 7;
        sampling.hitSpots = 2;

        GpuFrameData frameData{};
        packProbeSampling(frameData, sampling);

        assert(frameData.probeShading[0] == 0.4f && frameData.probeShading[1] == 0.9f);
        // The atlases' grid for every cascade's probes together (ddgi.h, probeAtlasGrid), and how many cascades.
        assert(frameData.probeAtlas[0] == 31 && frameData.probeAtlas[1] == 31 && frameData.probeAtlas[3] == 3);
        assert(frameData.probeLights[0] == 7 && frameData.probeLights[1] == 2 && frameData.probeLights[2] == 3);

        // More lights per hit than the shader holds are capped.
        sampling.lightsPerHit = 50;
        packProbeSampling(frameData, sampling);
        assert(frameData.probeLights[2] == maxLightsPerHit);
    }

    // Probe light that equals the ambient in both parts shades exactly like the ambient, up to rounding.
    void testProbeAmbientMatchesTheFlatAmbient() {
        const BrdfTable table = buildBrdfTable();
        const SurfaceProperties surface{Vec3{0.6f, 0.4f, 0.3f}, 0.2f, 0.5f};
        const Vec3 normal{0.0f, 1.0f, 0.0f};
        const ShadingPoint point = prepareShading(surface, normal, Vec3{0.6f, 0.8f, 0.0f}, table);
        const Vec3 ambient{0.5f, 0.7f, 0.9f};

        assert(nearlyEqual(shadeProbeAmbient(point, ambient, ambient), shadeAmbient(point, ambient)));

        // Each light reaches only its own part.
        const Vec3 doubledDiffuse = shadeProbeAmbient(point, ambient * 2.0f, ambient);
        assert(nearlyEqual(doubledDiffuse - shadeAmbient(point, ambient), ambient * point.diffuse));
        const Vec3 doubledSpecular = shadeProbeAmbient(point, ambient, ambient * 2.0f);
        assert(nearlyEqual(doubledSpecular - shadeAmbient(point, ambient), ambient * point.specularAlbedo));
    }
}

void testDdgiSampling() {
    testSelfShadowBiasLeansTowardsTheViewer();
    testTheCellAroundAPoint();
    testTrilinearWeightsInterpolateLinearly();
    testProbesBehindTheSurfaceCountLess();
    testChebyshevVisibility();
    testSmallWeightsAreCrushed();
    testACornersWholeWeight();
    testTheCascadeWeightFadesRoundItsCentre();
    testTheBlendAveragesSquareRoots();
    testUniformProbesGiveTheirOwnValue();
    testFastLightIsBlendedApart();
    testSpecularReadsTheMirrorDirection();
    testOnlyTracedProbesLight();
    testAWallKeepsTheDarkSidesProbesOut();
    testTheBrightestLightsAreKept();
    testLightAtAProbeRaysHit();
    testTheLightsNearTheProbesAreListed();
    testProbeSamplingIsPacked();
    testProbeAmbientMatchesTheFlatAmbient();
}
