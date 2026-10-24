#include "ecs/world.h"
#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/volumetric_fog.h"
#include "scene/camera.h"
#include "scene/fog.h"
#include "scene/lighting.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    bool rejects(const FogSettings& settings) {
        try {
            validateFog(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // A camera 1.7 m up looking north and a little down, as the grid would see the world.
    Camera testCamera() {
        return Camera{Vec3{3.0f, 1.7f, -2.0f}, Vec3{3.0f, 0.5f, -40.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 16.0f / 9.0f, 0.1f, 500.0f};
    }

    void testSettings() {
        const FogSettings defaults;
        assert(defaults.enabled && defaults.range == 100.0f && defaults.historyWeight == 0.9f && defaults.probeLight);
        assert(defaults.gridWidth == 160 && defaults.gridHeight == 90 && defaults.slices == 64);
        validateFog(defaults);

        FogSettings wrong;
        wrong.range = 1.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.range = std::numeric_limits<float>::quiet_NaN();
        assert(rejects(wrong));
        wrong = {};
        wrong.gridWidth = FOG_MAX_GRID_SIDE + 1;
        assert(rejects(wrong));
        wrong = {};
        wrong.slices = 1;
        assert(rejects(wrong));
        wrong = {};
        wrong.historyWeight = 1.0f;
        assert(rejects(wrong));
    }

    // The medium: the game's density at the base height, falling by e every scale height, more below.
    void testDensity() {
        Fog fog;
        fog.density = 0.05f;
        fog.baseHeight = 2.0f;
        fog.scaleHeight = 8.0f;
        assert(near(fogDensity(fog, 2.0f), 0.05f, 1e-7f));
        assert(near(fogDensity(fog, 10.0f), 0.05f / std::exp(1.0f), 1e-6f));
        assert(fogDensity(fog, 0.0f) > 0.05f);
    }

    /*
    * The optical depth along a ray through the height fog has a closed form: across, the density times the length;
    * straight up from the base to far away, the density times the scale height (the area under the exponential); at a
    * slant, the same as summing the density along the ray in fine steps. Looking down a long way stays finite.
    */
    void testOpticalDepth() {
        Fog fog;
        fog.density = 0.02f;
        fog.scaleHeight = 10.0f;

        assert(near(fogOpticalDepth(fog, 5.0f, 0.0f, 3.0f, 53.0f), fogDensity(fog, 5.0f) * 50.0f, 1e-6f));
        assert(near(fogOpticalDepth(fog, 0.0f, 1.0f, 0.0f, 10000.0f), 0.02f * 10.0f, 1e-5f));

        for (float directionY : {0.3f, -0.2f, 1e-5f, -1e-5f}) {
            // Summed in doubles: 20,000 float additions would lose the digits being checked.
            double summed = 0.0;
            const int steps = 20000;
            const double start = 2.0;
            const double end = 80.0;
            const double step = (end - start) / steps;

            for (int i = 0; i < steps; ++i) {
                summed += fogDensity(fog, 1.0f + directionY * static_cast<float>(start + (i + 0.5) * step)) * step;
            }

            const float expected = static_cast<float>(summed);
            assert(near(fogOpticalDepth(fog, 1.0f, directionY, 2.0f, 80.0f), expected, 1e-4f * expected + 1e-7f));
        }

        assert(fogOpticalDepth(fog, 1.0f, 0.3f, 10.0f, 10.0f) == 0.0f);
        const float downwards = fogOpticalDepth(fog, 1.0f, -1.0f, 0.0f, 100000.0f);
        assert(std::isfinite(downwards) && downwards > 1000.0f);
    }

    // The slices: from the camera to the grid's range, thin near the camera where a metre covers much of the screen.
    void testSlices() {
        const float range = 100.0f;
        assert(fogSliceDepth(0.0f, range) == 0.0f);
        assert(near(fogSliceDepth(1.0f, range), range, 1e-3f));

        float last = -1.0f;

        for (float slice = 0.0f; slice <= 1.0f; slice += 0.05f) {
            const float depth = fogSliceDepth(slice, range);
            assert(depth > last);
            assert(near(fogDepthSlice(depth, range), slice, 1e-5f));
            last = depth;
        }

        const float first = fogSliceDepth(1.0f / 64.0f, range);
        const float lastSlice = range - fogSliceDepth(63.0f / 64.0f, range);
        assert(first < 0.1f && lastSlice > 5.0f);
        assert(fogDepthSlice(2.0f * range, range) > 1.0f);
    }

    /*
    * A point and its place in the grid: across and down the screen (rows from the top, as textures run), and the slice
    * of its view depth. The point back from the place is the same point.
    */
    void testVolumeCoordinates() {
        const Camera camera = testCamera();
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const float range = 100.0f;
        const Vec3 forward = camera.getForward();

        const Vec3 ahead = fogVolumeCoordinate(forward * 20.0f, viewProjection, range);
        assert(near(ahead.x, 0.5f, 1e-5f) && near(ahead.y, 0.5f, 1e-5f));
        assert(near(ahead.z, fogDepthSlice(20.0f, range), 1e-5f));

        const Vec3 above = fogVolumeCoordinate(forward * 20.0f + camera.getUp() * 2.0f, viewProjection, range);
        assert(above.y < 0.5f);
        const Vec3 right = fogVolumeCoordinate(forward * 20.0f + camera.getRight() * 2.0f, viewProjection, range);
        assert(right.x > 0.5f);

        for (const Vec3& coordinate : {Vec3{0.5f, 0.5f, 0.3f}, Vec3{0.1f, 0.9f, 0.7f}, Vec3{0.95f, 0.02f, 0.01f}, Vec3{0.3f, 0.6f, 1.0f}}) {
            const Vec3 point = fogVolumePoint(coordinate, inverseViewProjection, forward, range);
            assert(near(point.dot(forward), fogSliceDepth(coordinate.z, range), 1e-3f * fogSliceDepth(coordinate.z, range) + 1e-5f));
            assert(nearVec(fogVolumeCoordinate(point, viewProjection, range), coordinate, 1e-4f));
        }
    }

    // The frames' offsets inside a froxel: within half of one each way, repeating every cycle, averaging to the middle.
    void testJitter() {
        Vec3 sum;

        for (std::uint64_t frame = 0; frame < fogJitterFrames; ++frame) {
            const Vec3 offset = fogJitter(frame);
            assert(std::abs(offset.x) < 0.5f && std::abs(offset.y) < 0.5f && std::abs(offset.z) < 0.5f);
            assert(nearVec(fogJitter(frame + fogJitterFrames), offset, 0.0f));
            sum = sum + offset;
        }

        assert(nearVec(sum * (1.0f / fogJitterFrames), Vec3{}, 0.07f));
        assert(!nearVec(fogJitter(0), fogJitter(1), 1e-3f));
    }

    /*
    * Light scattered towards the eye: a light's irradiance times pi times the phase function (the engine's pixels are pi
    * times radiance). Even scattering (g 0) sends a quarter of the irradiance, forward scattering more when looking
    * towards the light; and a uniform sky of radiance L, summed over every direction, gives pi L whatever g (the phase
    * function spreads light without making or losing any).
    */
    void testInScattering() {
        const Vec3 irradiance{2.0f, 1.0f, 4.0f};
        const Vec3 up{0.0f, 1.0f, 0.0f};
        assert(nearVec(fogInScattering(irradiance, up, Vec3{1.0f, 0.0f, 0.0f}, 0.0f), irradiance * 0.25f, 1e-6f));

        const Vec3 towards = fogInScattering(irradiance, up, up, 0.6f);
        const Vec3 away = fogInScattering(irradiance, up, up * -1.0f, 0.6f);
        assert(towards.x > 10.0f * away.x);

        for (float g : {0.0f, 0.6f, -0.3f}) {
            const int directions = 8192;
            const float radiance = 1.5f;
            Vec3 sum;

            for (int i = 0; i < directions; ++i) {
                const Vec3 incoming = sphericalFibonacci(i, directions);
                sum = sum + fogInScattering(Vec3{1.0f, 1.0f, 1.0f} * (radiance * 4.0f * pi / directions), incoming, Vec3{0.0f, 0.0f, 1.0f}, g);
            }

            assert(near(sum.x, pi * radiance, 0.01f * pi * radiance));
        }

        // The probes' light from above and below: a uniform sky (irradiance pi L each way) gives pi L.
        assert(nearVec(fogAmbient(Vec3{3.0f, 3.0f, 3.0f}, Vec3{3.0f, 3.0f, 3.0f}), Vec3{3.0f, 3.0f, 3.0f}, 1e-6f));
        assert(nearVec(fogAmbient(Vec3{4.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, 2.0f}), Vec3{2.0f, 2.0f, 1.0f}, 1e-6f));
    }

    /*
    * A slab of constant fog: the light it lets through is exp(-extinction x length), and the light it adds is the
    * scattered light times the share it stops, so one thick slab gives the same as many thin ones (energy-conserving),
    * and a thin slab adds the scattered light times its optical depth. Clear air adds nothing and dims nothing.
    */
    void testSlab() {
        const Vec3 scattered{0.4f, 0.5f, 0.6f};
        FogIntegration thick;
        fogIntegrateSlice(thick, scattered, 0.05f, 30.0f);
        assert(near(thick.transmittance, std::exp(-1.5f), 1e-6f));
        assert(nearVec(thick.light, scattered * (1.0f - std::exp(-1.5f)), 1e-6f));

        FogIntegration thin;

        for (int i = 0; i < 64; ++i) {
            fogIntegrateSlice(thin, scattered, 0.05f, 30.0f / 64.0f);
        }

        assert(near(thin.transmittance, thick.transmittance, 1e-5f));
        assert(nearVec(thin.light, thick.light, 1e-5f));

        FogIntegration sliver;
        fogIntegrateSlice(sliver, scattered, 1e-6f, 0.5f);
        assert(nearVec(sliver.light, scattered * 5e-7f, 1e-10f));

        FogIntegration clear;
        fogIntegrateSlice(clear, scattered, 0.0f, 10.0f);
        assert(clear.transmittance == 1.0f && clear.light.x == 0.0f);

        // A stretch of fog followed by another is the two in turn.
        FogIntegration first;
        FogIntegration second;
        FogIntegration both;
        fogIntegrateSlice(first, scattered, 0.03f, 10.0f);
        fogIntegrateSlice(second, Vec3{1.0f, 0.0f, 2.0f}, 0.1f, 4.0f);
        fogIntegrateSlice(both, scattered, 0.03f, 10.0f);
        fogIntegrateSlice(both, Vec3{1.0f, 0.0f, 2.0f}, 0.1f, 4.0f);
        const FogIntegration followed = fogFollow(first, second);
        assert(near(followed.transmittance, both.transmittance, 1e-6f));
        assert(nearVec(followed.light, both.light, 1e-6f));
    }

    /*
    * The picture through the fog: a surface dimmed by what the fog lets through, plus the light it adds. Glass is drawn
    * over a picture already fogged up to the surface behind it, so it adds its own light dimmed by the fog in front, and
    * the fog's light only for what it does not let through: the same as the glass and the surface behind fogged in turn.
    */
    void testCompositing() {
        FogIntegration front;
        fogIntegrateSlice(front, Vec3{0.3f, 0.3f, 0.35f}, 0.04f, 12.0f);
        FogIntegration behind;
        fogIntegrateSlice(behind, Vec3{0.2f, 0.25f, 0.3f}, 0.04f, 30.0f);
        const Vec3 surface{1.0f, 0.5f, 0.25f};
        assert(nearVec(fogComposite(surface, front), surface * front.transmittance + front.light, 1e-7f));

        const Vec3 glassAdded{0.1f, 0.2f, 0.05f};
        const Vec3 glassLetsThrough{0.8f, 0.7f, 0.9f};
        const Vec3 behindGlass = fogComposite(surface, fogFollow(front, behind));
        const Vec3 drawn = fogOverGlass(glassAdded, glassLetsThrough, front) + glassLetsThrough * behindGlass;
        const Vec3 expected = front.light + (glassAdded + glassLetsThrough * fogComposite(surface, behind)) * front.transmittance;
        assert(nearVec(drawn, expected, 1e-6f));
    }

    /*
    * Reading the integrated grid at a slice: texel k holds the fog from the camera to the far side of slice k, so a slice
    * coordinate is read half a texel earlier, and inside the first half slice the reading fades towards no fog at all.
    */
    void testSliceRead() {
        const FogSliceRead whole = fogSliceRead(1.0f, 64);
        assert(near(whole.z, 1.0f - 0.5f / 64.0f, 1e-7f) && whole.share == 1.0f);
        assert(fogSliceRead(2.0f, 64).share == 1.0f);
        assert(fogSliceRead(0.0f, 64).share == 0.0f);
        assert(near(fogSliceRead(0.25f / 64.0f, 64).share, 0.25f, 1e-6f));
        assert(near(fogSliceRead(0.5f, 64).z, 0.5f - 0.5f / 64.0f, 1e-7f));
    }

    // Last frame's grid counts only where the point was inside it.
    void testHistoryWeight() {
        assert(fogHistoryWeight(Vec3{0.5f, 0.3f, 0.7f}, 0.9f) == 0.9f);
        assert(fogHistoryWeight(Vec3{-0.01f, 0.3f, 0.7f}, 0.9f) == 0.0f);
        assert(fogHistoryWeight(Vec3{0.5f, 1.01f, 0.7f}, 0.9f) == 0.0f);
        assert(fogHistoryWeight(Vec3{0.5f, 0.3f, 1.2f}, 0.9f) == 0.0f);
    }

    /*
    * The integration pass's column, mirrored: each slice's length along the ray (its depth step stretched by the ray's
    * slant), integrated front to back. Uniform fog keeps exp(-extinction x the ray's length to the range).
    */
    void testColumn() {
        const float range = 60.0f;
        const float stretch = 1.25f;
        std::vector<Vec4> lighting(32, Vec4{0.5f, 0.5f, 0.5f, 0.02f});
        const std::vector<FogIntegration> column = integrateFogColumn(lighting, range, stretch);
        assert(column.size() == 32);
        assert(near(column.back().transmittance, std::exp(-0.02f * range * stretch), 1e-5f));
        assert(near(column[15].transmittance, std::exp(-0.02f * fogSliceDepth(16.0f / 32.0f, range) * stretch), 1e-5f));
        assert(near(column.back().light.x, 0.5f * (1.0f - column.back().transmittance), 1e-5f));

        for (std::size_t k = 1; k < column.size(); ++k) {
            assert(column[k].transmittance < column[k - 1].transmittance);
        }
    }

    // Past the grid, the height fog's closed form lit by one source: across, uniform fog's slab; nothing past its end.
    void testBeyondTheGrid() {
        Fog fog;
        fog.density = 0.01f;
        const Vec3 source{0.6f, 0.7f, 0.8f};
        const FogIntegration across = fogBeyondGrid(fog, source, fog.baseHeight, 0.0f, 100.0f, 400.0f);
        assert(near(across.transmittance, std::exp(-3.0f), 1e-5f));
        assert(nearVec(across.light, source * (1.0f - std::exp(-3.0f)), 1e-5f));

        const FogIntegration none = fogBeyondGrid(fog, source, 0.0f, 0.0f, 100.0f, 80.0f);
        assert(none.transmittance == 1.0f && none.light.x == 0.0f);
    }

    // A probe's weight for a point in the air: no surface faces it, so only its visibility and nearness count.
    void testProbeAirWeight() {
        const Vec2 seesPast{5.0f, 25.0f};
        assert(near(probeAirWeight(2.0f, seesPast, 0.4f), 0.4f, 1e-6f));
        const Vec2 wallBetween{1.0f, 1.01f};
        assert(probeAirWeight(3.0f, wallBetween, 0.4f) < 0.01f);
        assert(probeAirWeight(3.0f, wallBetween, 0.4f) > 0.0f);
    }

    /*
    * The frame's fog record: off without a Fog or with the setting off; otherwise the grid, the range (no further than
    * the camera's far plane, past which the clusters list no lights), the medium, and last frame's grid only when it is
    * usable.
    */
    void testPacking() {
        const Mat4 viewProjection = cameraRelativeViewProjection(testCamera());
        const FogSettings settings;
        Fog fog;
        fog.density = 0.03f;
        fog.anisotropy = 0.5f;
        fog.albedo = Vec3{0.9f, 0.8f, 0.7f};

        const GpuFog none = packFog(nullptr, settings, viewProjection, viewProjection, true, 0, 500.0f);
        assert(none.options[0] == 0.0f);
        FogSettings off;
        off.enabled = false;
        assert(packFog(&fog, off, viewProjection, viewProjection, true, 0, 500.0f).options[0] == 0.0f);

        const GpuFog packed = packFog(&fog, settings, viewProjection, viewProjection, true, 3, 500.0f);
        assert(packed.options[0] == 1.0f && packed.options[1] == settings.historyWeight && packed.options[2] == 1.0f);
        assert(packed.grid[0] == 160.0f && packed.grid[1] == 90.0f && packed.grid[2] == 64.0f && packed.grid[3] == 100.0f);
        assert(packed.medium[0] == 0.03f && packed.medium[3] == 0.5f && packed.albedo[2] == 0.7f && packed.albedo[3] == fog.maxDistance);
        assert(nearVec(Vec3{packed.jitter[0], packed.jitter[1], packed.jitter[2]}, fogJitter(3), 0.0f));
        assert(packFog(&fog, settings, viewProjection, viewProjection, false, 3, 500.0f).options[1] == 0.0f);
        assert(packFog(&fog, settings, viewProjection, viewProjection, true, 3, 60.0f).grid[3] == 60.0f);
    }

    // A Fog component reaches the frame's lighting (the first, if several).
    void testCollected() {
        const Fog defaults;
        assert(defaults.density == 0.02f && defaults.scaleHeight == 10.0f && defaults.anisotropy == 0.6f);

        World world;
        assert(!collectLighting(world, 1.0f).fog.has_value());

        Fog thick;
        thick.density = 0.2f;
        world.add(world.create(), thick);
        world.add(world.create(), Fog{});
        const FrameLighting lighting = collectLighting(world, 1.0f);
        assert(lighting.fog.has_value() && lighting.fog->density == 0.2f);
    }
}

void testVolumetricFog() {
    testSettings();
    testDensity();
    testOpticalDepth();
    testSlices();
    testVolumeCoordinates();
    testJitter();
    testInScattering();
    testSlab();
    testCompositing();
    testSliceRead();
    testHistoryWeight();
    testColumn();
    testBeyondTheGrid();
    testProbeAirWeight();
    testPacking();
    testCollected();
}
