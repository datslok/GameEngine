#include "math/mat4.h"
#include "core/dither.h"
#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/taa.h"
#include "scene/camera.h"

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

    Vec3 unit(const Vec3& v) {
        return v * (1.0f / v.length());
    }

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    bool rejects(const AmbientOcclusionSettings& settings) {
        try {
            validateAmbientOcclusion(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    void testSettings() {
        const AmbientOcclusionSettings defaults;
        assert(defaults.enabled && defaults.radius == 0.5f && defaults.falloff == 0.6f);
        assert(defaults.slices == 3 && defaults.steps == 6 && defaults.multipleBounces && defaults.specularOcclusion);
        assert(defaults.thickness == 0.25f && defaults.indirect && defaults.indirectRadius == 2.0f && defaults.indirectBrightest == 8.0f);
        validateAmbientOcclusion(defaults);

        AmbientOcclusionSettings wrong;
        wrong.radius = 0.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.falloff = 0.0f;
        assert(rejects(wrong));
        wrong.falloff = std::numeric_limits<float>::quiet_NaN();
        assert(rejects(wrong));
        wrong = {};
        wrong.slices = AO_MAX_SLICES + 1;
        assert(rejects(wrong));
        wrong = {};
        wrong.steps = 0;
        assert(rejects(wrong));
        wrong = {};
        wrong.thickness = 0.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.indirectRadius = std::numeric_limits<float>::quiet_NaN();
        assert(rejects(wrong));
        wrong = {};
        wrong.indirectBrightest = 0.0f;
        assert(rejects(wrong));

        assert(defaults.temporal && defaults.historyFrames == 16 && defaults.historyDepthShare == 0.05f);
        wrong = {};
        wrong.historyFrames = 0;
        assert(rejects(wrong));
        wrong.historyFrames = AO_MAX_HISTORY_FRAMES + 1;
        assert(rejects(wrong));
        wrong = {};
        wrong.historyDepthShare = 0.0f;
        assert(rejects(wrong));
        wrong.historyDepthShare = std::numeric_limits<float>::quiet_NaN();
        assert(rejects(wrong));
    }

    // Full weight up to the falloff's start, half way through it half, nothing at the radius and past it.
    void testOccluderWeight() {
        assert(occluderWeight(0.0f, 0.5f, 0.6f) == 1.0f);
        assert(occluderWeight(0.2f, 0.5f, 0.6f) == 1.0f);
        assert(near(occluderWeight(0.35f, 0.5f, 0.6f), 0.5f, 1e-5f));
        assert(occluderWeight(0.5f, 0.5f, 0.6f) == 0.0f);
        assert(occluderWeight(3.0f, 0.5f, 0.6f) == 0.0f);
    }

    // The closed form is the integral of cos(theta - n) |sin theta| from h0 to h1 (|sin theta| the slices' share of the
    // sphere): checked by quadrature.
    void testSliceClosedForm() {
        const Vec3 toCamera{0.0f, 0.0f, 1.0f};
        const Vec3 direction{1.0f, 0.0f, 0.0f};

        for (float tilt : {0.0f, 0.4f, -0.9f, 1.2f}) {
            const Vec3 normal{std::sin(tilt), 0.0f, std::cos(tilt)};
            const AoSlice slice = aoSlice(normal, toCamera, direction);
            assert(near(slice.normalAngle, tilt, 1e-5f));
            assert(near(slice.projectedLength, 1.0f, 1e-5f));

            for (float h0 : {-1.4f, -0.6f, 0.0f}) {
                for (float h1 : {0.1f, 0.8f, 1.5f}) {
                    // Horizons inside the normal's hemisphere, as the clamp leaves them.
                    const float low = std::max(h0, tilt - pi * 0.5f);
                    const float high = std::min(h1, tilt + pi * 0.5f);
                    double integral = 0.0;
                    const int steps = 20000;

                    for (int i = 0; i < steps; ++i) {
                        const double theta = low + (high - low) * (i + 0.5) / steps;
                        integral += std::cos(theta - tilt) * std::abs(std::sin(theta));
                    }

                    integral *= (high - low) / steps;
                    const float visibility = sliceVisibility(slice, std::cos(h1), std::cos(-h0));
                    assert(near(visibility, static_cast<float>(integral), 1e-3f));
                }
            }
        }
    }

    // Nothing above the surface: averaged over slices turned all round the way to the camera, exactly 1, the normal
    // tilted or not (the projected length and the closed form between them weigh the slices right).
    void testOpenSurfaceIsFullyVisible() {
        const Vec3 toCamera = unit(Vec3{0.2f, 0.3f, 1.0f});

        for (const Vec3& normal : {toCamera, unit(Vec3{0.0f, 1.0f, 0.4f}), unit(Vec3{-0.7f, 0.1f, 0.5f})}) {
            const Vec3 across = unit(toCamera.cross(Vec3{0.0f, 1.0f, 0.0f}));
            const Vec3 other = toCamera.cross(across);
            const int slices = 2000;
            double sum = 0.0;

            for (int k = 0; k < slices; ++k) {
                const float angle = pi * (static_cast<float>(k) + 0.5f) / slices;
                const AoSlice slice = aoSlice(normal, toCamera, across * std::cos(angle) + other * std::sin(angle));
                sum += sliceVisibility(slice, lowHorizonCosine(slice, 0), lowHorizonCosine(slice, 1));
            }

            assert(near(static_cast<float>(sum / slices), 1.0f, 2e-3f));
        }
    }

    // A surface facing the camera with everything blocked to the zenith on one side of every slice (a wall of infinite
    // height beside it, seen from above): half the light from all around.
    void testHalfBlocked() {
        const Vec3 up{0.0f, 0.0f, 1.0f};
        const AoSlice slice = aoSlice(up, up, Vec3{1.0f, 0.0f, 0.0f});
        assert(near(sliceVisibility(slice, 1.0f, lowHorizonCosine(slice, 1)), 0.5f, 1e-5f));
        assert(near(sliceVisibility(slice, 1.0f, 1.0f), 0.0f, 1e-5f));
        assert(near(horizonCosine(Vec3{1.0f, 0.0f, 1.0f}, up), std::sqrt(0.5f), 1e-6f));
    }

    // A sector's place is its share of the slice's cosine-weighted light, so equal sectors weigh the same: 0 and 1 at
    // the surface's two horizons, and in between the closed form's share, checked by quadrature.
    void testSectorShare() {
        const Vec3 toCamera{0.0f, 0.0f, 1.0f};
        const Vec3 direction{1.0f, 0.0f, 0.0f};

        for (float tilt : {0.0f, 0.4f, -0.9f, 1.2f}) {
            const AoSlice slice = aoSlice(Vec3{std::sin(tilt), 0.0f, std::cos(tilt)}, toCamera, direction);
            assert(near(sectorShare(slice, lowHorizonCosine(slice, 0), 0), 1.0f, 1e-4f));
            assert(near(sectorShare(slice, lowHorizonCosine(slice, 1), 1), 0.0f, 1e-4f));
            const float low = tilt - pi * 0.5f;
            const float high = tilt + pi * 0.5f;
            const auto weightBetween = [tilt](float from, float to) {
                double integral = 0.0;
                const int steps = 20000;

                for (int i = 0; i < steps; ++i) {
                    const double theta = from + (to - from) * (i + 0.5) / steps;
                    integral += std::cos(theta - tilt) * std::abs(std::sin(theta));
                }

                return static_cast<float>(integral * (to - from) / steps);
            };
            const float total = weightBetween(low, high);

            for (float share : {0.15f, 0.5f, 0.85f}) {
                const float angle = low + (high - low) * share;
                const int side = angle < 0.0f ? 1 : 0;
                assert(near(sectorShare(slice, std::cos(angle), side), weightBetween(low, angle) / total, 1e-3f));
            }
        }

        // Below the surface counts as the horizon, and side 1 lies before side 0.
        const AoSlice flat = aoSlice(toCamera, toCamera, direction);
        assert(sectorShare(flat, -0.5f, 0) == 1.0f && sectorShare(flat, -0.5f, 1) == 0.0f);
        assert(sectorShare(flat, 0.5f, 1) < sectorShare(flat, 1.0f, 0) && sectorShare(flat, 1.0f, 0) < sectorShare(flat, 0.5f, 0));
    }

    // A sample no brighter than the limit is left alone; a brighter one (the sun's glint off a metal ball) comes down to
    // it keeping its colour, so one pixel cannot speckle what it lights.
    void testGatheredSampleLight() {
        assert(nearVec(gatheredSampleLight(Vec3{1.0f, 2.0f, 0.5f}, 8.0f), Vec3{1.0f, 2.0f, 0.5f}, 0.0f));
        assert(nearVec(gatheredSampleLight(Vec3{8.0f, 2.0f, 0.0f}, 8.0f), Vec3{8.0f, 2.0f, 0.0f}, 0.0f));
        assert(nearVec(gatheredSampleLight(Vec3{80.0f, 20.0f, 40.0f}, 8.0f), Vec3{8.0f, 2.0f, 4.0f}, 1e-5f));
        assert(nearVec(gatheredSampleLight(Vec3{}, 8.0f), Vec3{}, 0.0f));
    }

    // The sectors an arc covers at least half of (the paper's round criterion), either way round.
    void testSectorMask() {
        assert(sectorMask(0.0f, 1.0f) == 0xFFFFFFFFu);
        assert(sectorMask(0.5f, 1.0f) == 0xFFFF0000u);
        assert(sectorMask(1.0f, 0.5f) == 0xFFFF0000u);
        assert(sectorMask(0.0f, 0.25f) == 0x000000FFu);
        assert(sectorMask(0.5f, 0.5f) == 0u);
        assert(sectorMask(0.5f, 0.505f) == 0u);
        assert(sectorMask(1.0f, 1.0f) == 0u);
        assert(sectorMask(0.5f, 0.53125f) == 0x00010000u);

        // The offset is where an end rounds: 0.5 as before; near 1 even a sliver marks its sector, near 0 hardly any.
        assert(sectorMask(0.5f, 0.505f, 0.5f) == sectorMask(0.5f, 0.505f));
        assert(sectorMask(0.0f, 0.01f, 0.99f) == 0x00000001u);
        assert(sectorMask(0.0f, 0.01f, 0.01f) == 0u);
        assert(sectorMask(0.0f, 1.0f, 0.3f) == 0xFFFFFFFFu);

        // Averaged over the offset the bits count the arc exactly, slivers and whole sectors alike, wherever it lies.
        for (const float start : {0.0f, 0.1f, 0.47f, 0.85f}) {
            for (const float length : {0.003f, 0.01f, 0.03f, 0.11f}) {
                const int offsets = 4096;
                double bits = 0.0;

                for (int i = 0; i < offsets; ++i) {
                    bits += std::popcount(sectorMask(start, start + length, (i + 0.5f) / offsets));
                }

                assert(near(static_cast<float>(bits / offsets), length * AO_SECTORS, 2e-3f));
            }
        }
    }

    /*
    * A plane's normal, facing the camera. At a wall's foot the pixel above is on the wall and nearer than the one below
    * on the foreshortened floor; the step that carries on in line keeps the floor's slope all the same. A neighbour off
    * screen (clamped onto the pixel) gives way to the other side.
    */
    void testDepthNormal() {
        const Vec3 centre{0.0f, -1.0f, -3.0f};
        const Vec3 across = surfaceStep(centre, Vec3{-0.05f, -1.0f, -3.0f}, Vec3{-0.1f, -1.0f, -3.0f}, Vec3{0.05f, -1.0f, -3.0f},
                                        Vec3{0.1f, -1.0f, -3.0f});
        const Vec3 floorDown{0.0f, -1.0f, -2.9f};
        const Vec3 floorDownNext{0.0f, -1.0f, -2.8f};
        const Vec3 wallUp{0.0f, -0.98f, -3.01f};
        const Vec3 wallUpNext{0.0f, -0.96f, -3.01f};
        const Vec3 downwards = surfaceStep(centre, wallUp, wallUpNext, floorDown, floorDownNext);
        assert(nearVec(downwards, floorDown - centre, 1e-6f));
        assert(nearVec(depthNormal(centre, across, downwards), Vec3{0.0f, 1.0f, 0.0f}, 1e-5f));

        const Vec3 clamped = surfaceStep(centre, centre, centre, floorDown, floorDownNext);
        assert(nearVec(clamped, floorDown - centre, 1e-6f));
        assert(nearVec(depthNormal(centre, Vec3{}, Vec3{}), Vec3{0.0f, 1.0f, 3.0f}.normalized(), 1e-5f));
    }

    // The radius on screen shrinks with distance and is capped; the steps grow quadratically from the smallest offset.
    void testScreenSteps() {
        const float footprint = 0.002f; // 2 mm a pixel at a metre
        assert(near(aoScreenRadius(0.5f, footprint, 2.0f), 125.0f, 1e-3f));
        assert(aoScreenRadius(0.5f, footprint, 0.1f) == static_cast<float>(AO_MAX_SCREEN_RADIUS));

        float last = 0.0f;

        for (int step = 0; step < 6; ++step) {
            const float distance = aoSampleDistance(step, 6, 0.5f, 100.0f);
            assert(distance >= static_cast<float>(AO_MIN_SAMPLE_PIXELS) && distance > last && distance <= 100.0f);
            last = distance;
        }

        assert(near(aoSampleDistance(2, 4, 1.0f, 100.0f), 56.25f, 1e-3f));
    }

    // Open stays open; light bouncing back into a crease brightens it more for bright walls, never darkening it.
    void testMultipleBounces() {
        assert(nearVec(multiBounceOcclusion(1.0f, Vec3{0.8f, 0.3f, 0.0f}), Vec3{1.0f, 1.0f, 1.0f}, 2e-3f));
        const Vec3 white = multiBounceOcclusion(0.5f, Vec3{0.9f, 0.9f, 0.9f});
        const Vec3 dark = multiBounceOcclusion(0.5f, Vec3{0.05f, 0.05f, 0.05f});
        assert(white.x > dark.x && dark.x >= 0.5f && white.x <= 1.0f);
        assert(multiBounceOcclusion(0.0f, Vec3{0.9f, 0.9f, 0.9f}).x == 0.0f);
    }

    // Open lets every reflection through and buried none; a crease dims a smooth surface's reflection less head-on.
    void testSpecularOcclusion() {
        assert(near(specularOcclusion(0.7f, 1.0f, 0.3f), 1.0f, 1e-6f));
        assert(specularOcclusion(0.7f, 0.0f, 0.3f) == 0.0f);
        const float headOn = specularOcclusion(1.0f, 0.5f, 0.1f);
        const float grazing = specularOcclusion(0.1f, 0.5f, 0.1f);
        assert(headOn > grazing && headOn <= 1.0f && grazing >= 0.0f);
    }

    // A material's baked occlusion (glTF: 1 + strength (texel - 1)) and the screen's both estimate how open the point
    // is, so the more closed one counts, not their product, which would darken a crease both see twice.
    void testSurfaceVisibility() {
        assert(surfaceVisibility(1.0f, 1.0f, 1.0f) == 1.0f);
        assert(surfaceVisibility(1.0f, 0.2f, 0.0f) == 1.0f);
        assert(near(surfaceVisibility(1.0f, 0.2f, 1.0f), 0.2f, 1e-6f));
        assert(near(surfaceVisibility(1.0f, 0.2f, 0.5f), 0.6f, 1e-6f));
        assert(near(surfaceVisibility(0.4f, 0.2f, 0.5f), 0.4f, 1e-6f));
        assert(near(surfaceVisibility(0.7f, 0.5f, 1.0f), 0.5f, 1e-6f));
    }

    /*
    * Over the frames a pixel's slice angles must meet every step stagger: the frames whose slice noise falls in one
    * quarter have step noises in all four quarters. Taking the stagger from the same noise at a shifted pixel (the first
    * way) ties it to the angle, and the same quarter of angles gets one narrow band of staggers.
    */
    void testNoisesIndependent() {
        for (const Vec2 pixel : {Vec2{100.5f, 70.5f}, Vec2{3.5f, 251.5f}, Vec2{611.5f, 12.5f}}) {
            int quarters[4] = {0, 0, 0, 0};
            int tiedQuarters[4] = {0, 0, 0, 0};
            int inQuarter = 0;
            int sectorBySlice[4] = {0, 0, 0, 0};
            int sectorByStep[4] = {0, 0, 0, 0};
            int inStepQuarter = 0;

            for (int frame = 0; frame < 1024; ++frame) {
                const Vec3 noise = aoNoise(pixel, frame);
                assert(noise.x >= 0.0f && noise.x < 1.0f && noise.y >= 0.0f && noise.y < 1.0f && noise.z >= 0.0f && noise.z < 1.0f);

                // The sectors' offset is independent of both others: for a quarter of either, all its quarters.
                if (noise.x < 0.25f) {
                    ++sectorBySlice[std::min(static_cast<int>(noise.z * 4.0f), 3)];
                }

                if (noise.y < 0.25f) {
                    ++inStepQuarter;
                    ++sectorByStep[std::min(static_cast<int>(noise.z * 4.0f), 3)];
                }

                if (noise.x < 0.25f) {
                    ++inQuarter;
                    ++quarters[std::min(static_cast<int>(noise.y * 4.0f), 3)];
                    const float shift = static_cast<float>(AO_NOISE_FRAME_SHIFT) * static_cast<float>(frame);
                    const float tied = interleavedGradientNoise(pixel.x + shift + 23.0f, pixel.y + shift + 41.0f);
                    ++tiedQuarters[std::min(static_cast<int>(tied * 4.0f), 3)];
                }
            }

            assert(inQuarter > 128);
            int tiedEmpty = 0;

            for (int quarter = 0; quarter < 4; ++quarter) {
                assert(quarters[quarter] > inQuarter / 8);
                assert(sectorBySlice[quarter] > inQuarter / 8);
                assert(sectorByStep[quarter] > inStepQuarter / 8);
                tiedEmpty += tiedQuarters[quarter] == 0 ? 1 : 0;
            }

            assert(tiedEmpty >= 2);
        }

        // The slices' turn is the noise moved along the screen each frame, as before.
        assert(aoNoise(Vec2{10.5f, 20.5f}, 0).x == interleavedGradientNoise(10.5f, 20.5f));
    }

    void testBlurWeight() {
        assert(aoBlurWeight(5.0f, 5.0f) == 1.0f);
        assert(near(aoBlurWeight(5.0f, 5.25f), 0.5f, 1e-5f));
        assert(aoBlurWeight(5.0f, 6.0f) == 0.0f);
    }

    // An eye 1.5 m above a floor looking down -z, tilted down at a wall 2 m high at z = -5, so the floor shows from about
    // 1.5 m out to the wall's foot, and the sky above the wall.
    constexpr RenderSize screenSize{640, 360};
    constexpr float wallZ = -5.0f;

    Camera makeCamera() {
        return Camera{Vec3{0.0f, 1.5f, 0.0f}, Vec3{0.0f, 0.3f, -4.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                      static_cast<float>(screenSize.width) / static_cast<float>(screenSize.height), 0.1f, 500.0f};
    }

    std::vector<float> drawDepths(const Mat4& viewProjection, const Mat4& inverseViewProjection) {
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        std::vector<float> depths(screenSize.width * screenSize.height, gpuFarDepth);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const Vec3 direction = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, 0.5f, size).normalized();
                std::optional<float> nearest;

                if (direction.y < 0.0f) {
                    nearest = -1.5f / direction.y;
                }

                // The wall stands 2 m high, the sky above it.
                if (direction.z < 0.0f && (!nearest || wallZ / direction.z < *nearest) && (direction * (wallZ / direction.z)).y + 1.5f <= 2.0f) {
                    nearest = wallZ / direction.z;
                }

                if (nearest) {
                    const Vec3 point = direction * *nearest;
                    const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                    depths[y * screenSize.width + x] = clip.z / clip.w;
                }
            }
        }

        return depths;
    }

    // The texel whose centre sees the point (measured from the camera).
    void texelOf(const Mat4& viewProjection, const Vec3& point, int& x, int& y) {
        const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
        x = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * screenSize.width);
        y = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * screenSize.height);
    }

    // The pass's estimate averaged over a grid of the noise each frame draws, as temporal anti-aliasing averages frames.
    float averagedOcclusion(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, const AoDepthImage& depthAt) {
        const int grid = 8;
        double sum = 0.0;

        for (int i = 0; i < grid; ++i) {
            for (int j = 0; j < grid; ++j) {
                sum += ambientOcclusionAt(x, y, frame, settings, (i + 0.5f) / grid, (j + 0.5f) / grid, depthAt).visibility;
            }
        }

        return static_cast<float>(sum / (grid * grid));
    }

    // The same with the light gathered from a picture.
    AoLight averagedLight(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, const AoDepthImage& depthAt, const AoPicture& picture) {
        const int grid = 8;
        AoLight sum;
        sum.visibility = 0.0f;

        for (int i = 0; i < grid; ++i) {
            for (int j = 0; j < grid; ++j) {
                const AoLight light = ambientOcclusionAt(x, y, frame, settings, (i + 0.5f) / grid, (j + 0.5f) / grid, depthAt, picture);
                sum.visibility += light.visibility / (grid * grid);
                sum.gathered = sum.gathered + light.gathered * (1.0f / (grid * grid));
            }
        }

        return sum;
    }

    /*
    * The loop through a drawn depth image: open floor far from the wall sees everything; the floor at the wall's foot,
    * with a radius reaching far up the wall, about half (a wall 2 m high, 3 cm away, blocks nearly half of the sky);
    * with the default radius it is darker there than a metre out, where nothing is within reach; the sky is open.
    */
    void testFloorAndWall() {
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const std::vector<float> depths = drawDepths(viewProjection, inverseViewProjection);
        const AoDepthImage depthAt = [&](int x, int y) { return depths[static_cast<std::size_t>(y) * screenSize.width + static_cast<std::size_t>(x)]; };
        const AoFrame frame{inverseViewProjection, screenSize, pixelFootprint(viewProjection, static_cast<float>(screenSize.height))};
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        const Vec3 eye{0.0f, 1.5f, 0.0f};

        int x = 0;
        int y = 0;
        AmbientOcclusionSettings settings;
        settings.radius = 0.5f;
        texelOf(viewProjection, Vec3{0.0f, 0.0f, -2.5f} - eye, x, y);
        assert(averagedOcclusion(x, y, frame, settings, depthAt) > 0.98f);

        texelOf(viewProjection, Vec3{0.0f, 0.0f, wallZ + 1.0f} - eye, x, y);
        const float metreOut = averagedOcclusion(x, y, frame, settings, depthAt);
        assert(metreOut > 0.98f);

        // The foot: up the middle column, the last floor texel before the wall.
        int foot = -1;

        for (int row = static_cast<int>(screenSize.height) - 1; row >= 0; --row) {
            const Vec3 point = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, row + 0.5f}, depthAt(x, row), size) + eye;

            if (std::abs(point.y) > 1e-3f) {
                break;
            }

            foot = row;
        }

        assert(foot > 0);
        const Vec3 footPoint = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, foot + 0.5f}, depthAt(x, foot), size) + eye;
        assert(footPoint.z - wallZ < 0.1f);
        const float atTheFoot = averagedOcclusion(x, foot, frame, settings, depthAt);
        assert(atTheFoot < 0.85f);
        y = foot;

        AmbientOcclusionSettings far;
        far.radius = 20.0f;
        far.falloff = 0.05f;
        far.slices = 8;
        far.steps = 16;
        const float half = averagedOcclusion(x, y, frame, far, depthAt);
        assert(near(half, 0.5f, 0.06f));

        const AoLight sky = ambientOcclusionAt(static_cast<int>(screenSize.width / 2), 0, frame, settings, 0.5f, 0.5f, depthAt);
        assert(sky.visibility == 1.0f && sky.gathered.x == 0.0f);

        /*
        * The light gathered: seen sectors and open ones share out the whole hemisphere, so a picture of 1 everywhere
        * (a white furnace) gives exactly the open light's missing share, and nothing is counted twice. It takes the
        * colour of what it sees, and open floor gathers nothing from itself.
        */
        const AoPicture white = [](int, int) { return Vec3{1.0f, 1.0f, 1.0f}; };
        const AoLight furnace = averagedLight(x, foot, frame, far, depthAt, white);
        assert(near(furnace.visibility + furnace.gathered.x, 1.0f, 0.02f));
        assert(furnace.gathered.x > 0.35f);

        // The same averaged over the sectors' rounding offset too: still exactly the missing light.
        {
            const int grid = 6;
            double total = 0.0;

            for (int i = 0; i < grid; ++i) {
                for (int j = 0; j < grid; ++j) {
                    for (int k = 0; k < grid; ++k) {
                        const AoLight light = ambientOcclusionAt(x, foot, frame, far, (i + 0.5f) / grid, (j + 0.5f) / grid, depthAt, white, (k + 0.5f) / grid);
                        total += light.visibility + light.gathered.x;
                    }
                }
            }

            assert(near(static_cast<float>(total / (grid * grid * grid)), 1.0f, 0.02f));
        }

        const AoPicture red = [](int, int) { return Vec3{0.8f, 0.0f, 0.0f}; };
        const AoLight tinted = averagedLight(x, foot, frame, far, depthAt, red);
        assert(near(tinted.gathered.x, furnace.gathered.x * 0.8f, 1e-3f) && tinted.gathered.y == 0.0f && tinted.gathered.z == 0.0f);

        const AoPicture blinding = [](int, int) { return Vec3{1000.0f, 1000.0f, 1000.0f}; };
        assert(averagedLight(x, foot, frame, far, depthAt, blinding).gathered.x <= furnace.gathered.x * far.indirectBrightest * 1.01f);

        // Open floor gathers nothing from itself.
        texelOf(viewProjection, Vec3{0.0f, 0.0f, -2.5f} - eye, x, y);
        assert(averagedLight(x, y, frame, settings, depthAt, white).gathered.x < 0.02f);
    }

    /*
    * A thin board 0.3 m in front of a wall the camera faces: a pixel on the wall beside it. Thin, the board blocks only
    * its own arc and light comes in behind it; assumed thick, it blocks everything behind it too, the halo a horizon
    * alone leaves.
    */
    void testThicknessLetsLightBehind() {
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                            static_cast<float>(screenSize.width) / static_cast<float>(screenSize.height), 0.1f, 500.0f};
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        std::vector<float> depths(screenSize.width * screenSize.height);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const Vec3 direction = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, 0.5f, size).normalized();
                const float height = direction.y * (-5.0f / direction.z);
                const float z = std::abs(height) < 0.1f ? -4.7f : -5.0f;
                const Vec3 point = direction * (z / direction.z);
                const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                depths[y * screenSize.width + x] = clip.z / clip.w;
            }
        }

        const AoDepthImage depthAt = [&](int x, int y) { return depths[static_cast<std::size_t>(y) * screenSize.width + static_cast<std::size_t>(x)]; };
        const AoFrame frame{inverseViewProjection, screenSize, pixelFootprint(viewProjection, static_cast<float>(screenSize.height))};
        int x = 0;
        int y = 0;
        texelOf(viewProjection, Vec3{0.0f, 0.2f, -5.0f}, x, y);

        AmbientOcclusionSettings thin;
        thin.radius = 1.0f;
        thin.slices = 8;
        thin.steps = 16;
        thin.thickness = 0.02f;
        AmbientOcclusionSettings thick = thin;
        thick.thickness = 5.0f;
        assert(averagedOcclusion(x, y, frame, thin, depthAt) > averagedOcclusion(x, y, frame, thick, depthAt) + 0.05f);
    }

    // The age climbs by one a frame up to the cap, and the new frame's share is one over it: the first frame alone, then
    // the exact mean, then a fixed share.
    void testHistoryAge() {
        assert(aoHistoryAge(0.0f, 16) == 1.0f);
        assert(aoHistoryAge(15.0f, 16) == 16.0f);
        assert(aoHistoryAge(16.0f, 16) == 16.0f);
        assert(aoHistoryAge(2.5f, 16) == 3.5f);
        assert(aoHistoryAge(40.0f, 1) == 1.0f);
        assert(aoCurrentShare(1.0f) == 1.0f);
        assert(aoCurrentShare(16.0f) == 1.0f / 16.0f);
        assert(aoCurrentShare(0.0f) == 1.0f);
    }

    // A texel shows the point when its depth is within the share of the expected one; the sky's (0) never does.
    void testHistoryTapKept() {
        assert(aoHistoryTapKept(10.0f, 10.0f, 0.05f));
        assert(aoHistoryTapKept(10.0f, 10.4f, 0.05f));
        assert(aoHistoryTapKept(10.0f, 9.6f, 0.05f));
        assert(!aoHistoryTapKept(10.0f, 10.6f, 0.05f));
        assert(!aoHistoryTapKept(10.0f, 9.0f, 0.05f));
        assert(!aoHistoryTapKept(10.0f, 0.0f, 0.05f));
    }

    // A camera at the origin looking down -z, or moved along x or z, at a wall at z = -5 filling the screen.
    Camera wallCamera(const Vec3& position) {
        return Camera{position, position + Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                      static_cast<float>(screenSize.width) / static_cast<float>(screenSize.height), 0.1f, 500.0f};
    }

    // The wall's depths seen from this camera, and where a nearer block (z from blockZ, x within blockHalfWidth of 0)
    // stands in front of it, the block's.
    std::vector<float> wallDepths(const Camera& camera, float blockZ = 0.0f, float blockHalfWidth = 0.0f) {
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        const Vec3 eye = camera.getPosition();
        std::vector<float> depths(screenSize.width * screenSize.height);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const Vec3 direction = positionAtPixel(inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, 0.5f, size).normalized();
                Vec3 point = direction * ((wallZ - eye.z) / direction.z);

                if (blockHalfWidth > 0.0f) {
                    const Vec3 onBlock = direction * ((blockZ - eye.z) / direction.z);

                    if (std::abs(onBlock.x + eye.x) < blockHalfWidth) {
                        point = onBlock;
                    }
                }

                const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                depths[y * screenSize.width + x] = clip.z / clip.w;
            }
        }

        return depths;
    }

    AoDepthImage depthImage(const std::vector<float>& depths) {
        return [&depths](int x, int y) { return depths[static_cast<std::size_t>(y) * screenSize.width + static_cast<std::size_t>(x)]; };
    }

    AoFrame wallFrame(const Camera& camera) {
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        return AoFrame{*inverse(viewProjection), screenSize, pixelFootprint(viewProjection, static_cast<float>(screenSize.height))};
    }

    // From this frame's clip space to last frame's, as the renderer makes it.
    Mat4 reprojectionBetween(const Camera& previous, const Camera& current) {
        const Mat4 previousRelative = previousRelativeViewProjection(cameraRelativeViewProjection(previous), previous.getPosition(), current.getPosition());
        return taaReprojection(cameraRelativeViewProjection(current), previousRelative);
    }

    // A whole image of history texels, as the temporal pass leaves them.
    using HistoryTexels = std::vector<AoHistoryTexel>;

    AoHistoryImage historyImage(const HistoryTexels& texels) {
        return [&texels](int x, int y) { return texels[static_cast<std::size_t>(y) * screenSize.width + static_cast<std::size_t>(x)]; };
    }

    // The point reprojects to its own pixel and depth with a still camera; moving the camera back a metre puts it a
    // metre further away; a point behind last frame's camera, or off its screen, has no place.
    void testPreviousPlace() {
        const Camera camera = wallCamera(Vec3{});
        const std::vector<float> depths = wallDepths(camera);
        const AoFrame frame = wallFrame(camera);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        const Vec2 centre{320.5f, 180.5f};
        const float depth = depths[180 * screenSize.width + 320];
        const float viewDepth = viewDistanceAtPixel(frame.inverseViewProjection, centre, depth, size);
        assert(near(viewDepth, 5.0f, 1e-3f));

        const AoPreviousPlace still = aoPreviousPlace(Mat4::identity(), centre, depth, viewDepth, size);
        assert(still.valid && still.pixel.x == centre.x && still.pixel.y == centre.y && still.viewDepth == viewDepth);

        // Further back, the pixel moves towards the screen's centre (320, 180) by a sixth of its half pixel.
        const AoPreviousPlace back = aoPreviousPlace(reprojectionBetween(wallCamera(Vec3{0.0f, 0.0f, 1.0f}), camera), centre, depth, viewDepth, size);
        assert(back.valid && near(back.viewDepth, 6.0f, 1e-3f) && near(back.pixel.x, 320.0f + 0.5f * 5.0f / 6.0f, 1e-2f));

        assert(!aoPreviousPlace(reprojectionBetween(wallCamera(Vec3{0.0f, 0.0f, -6.0f}), camera), centre, depth, viewDepth, size).valid);
        assert(!aoPreviousPlace(reprojectionBetween(wallCamera(Vec3{30.0f, 0.0f, 0.0f}), camera), centre, depth, viewDepth, size).valid);
    }

    // The new frame alone without history; then each frame's share one over the age, so the first frames give exactly
    // their mean, and with the history full a fixed share.
    void testAccumulateAo() {
        const AoHistoryTexel first = accumulateAo(Vec4{0.2f, 0.4f, 0.6f, 0.5f}, 5.0f, AoHistory{}, 16);
        assert(first.age == 1.0f && first.viewDepth == 5.0f && first.light.x == 0.2f && first.light.w == 0.5f);

        const AoHistoryTexel second = accumulateAo(Vec4{0.4f, 0.0f, 0.0f, 1.0f}, 5.0f, AoHistory{first.light, first.age}, 16);
        assert(second.age == 2.0f && near(second.light.x, 0.3f, 1e-6f) && near(second.light.w, 0.75f, 1e-6f));

        const AoHistoryTexel full = accumulateAo(Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 5.0f, AoHistory{Vec4{}, 16.0f}, 16);
        assert(full.age == 16.0f && near(full.light.x, 1.0f / 16.0f, 1e-6f));
    }

    // A noisy result each frame (the same mean everywhere) with a still camera: the average holds the exact mean of the
    // frames so far while it grows, then settles near the true mean with far less spread than one frame's.
    void testStillCameraConverges() {
        const Camera camera = wallCamera(Vec3{});
        const std::vector<float> depths = wallDepths(camera);
        const AoDepthImage depthAt = depthImage(depths);
        const AoFrame frame = wallFrame(camera);
        const AmbientOcclusionSettings settings;
        const int x = 100;
        const int y = 70;
        HistoryTexels history(screenSize.width * screenSize.height);

        // A fixed sequence of noise round 0.5, spread 0.29 (uniform on 0..1).
        std::uint32_t state = 12345u;
        const auto noise = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / 16777216.0f;
        };

        double sum = 0.0;
        double spread = 0.0;
        int counted = 0;

        for (int frameNumber = 0; frameNumber < 400; ++frameNumber) {
            const float value = noise();
            const AoHistoryTexel next = aoTemporalAt(x, y, frame, Mat4::identity(), settings, depthAt, Vec4{value, value, value, 1.0f}, historyImage(history),
                                                     frameNumber > 0);
            sum += value;

            if (frameNumber < settings.historyFrames) {
                assert(next.age == static_cast<float>(frameNumber + 1));
                assert(near(next.light.x, static_cast<float>(sum / (frameNumber + 1)), 1e-5f));
            } else {
                assert(next.age == static_cast<float>(settings.historyFrames));
            }

            if (frameNumber >= 100) {
                spread += (next.light.x - 0.5) * (next.light.x - 0.5);
                ++counted;
            }

            history[static_cast<std::size_t>(y) * screenSize.width + static_cast<std::size_t>(x)] = next;
        }

        // One frame's spread is 0.29; sixteen frames' exponential average about 0.29 / sqrt(31) = 0.05.
        assert(std::sqrt(spread / counted) < 0.08);
    }

    // A white furnace stays exactly white whatever the camera does, and without a usable history the frame stands alone.
    void testFurnaceAndNoHistory() {
        const Camera previous = wallCamera(Vec3{});
        const Camera current = wallCamera(Vec3{0.13f, 0.05f, 0.2f});
        const std::vector<float> depths = wallDepths(current);
        const std::vector<float> previousDepths = wallDepths(previous);
        const AoFrame previousFrame = wallFrame(previous);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        HistoryTexels history(screenSize.width * screenSize.height);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const float depth = previousDepths[y * screenSize.width + x];
                history[y * screenSize.width + x] =
                    AoHistoryTexel{Vec4{1.0f, 1.0f, 1.0f, 1.0f}, viewDistanceAtPixel(previousFrame.inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, depth, size), 16.0f};
            }
        }

        const AmbientOcclusionSettings settings;
        const Vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
        const Mat4 reprojection = reprojectionBetween(previous, current);
        const AoFrame frame = wallFrame(current);

        for (const Vec2 pixel : {Vec2{0.0f, 0.0f}, Vec2{320.0f, 180.0f}, Vec2{639.0f, 359.0f}, Vec2{7.0f, 300.0f}}) {
            const AoHistoryTexel texel = aoTemporalAt(static_cast<int>(pixel.x), static_cast<int>(pixel.y), frame, reprojection, settings, depthImage(depths), white,
                                                      historyImage(history), true);
            assert(near(texel.light.x, 1.0f, 1e-5f) && near(texel.light.w, 1.0f, 1e-5f));
        }

        const AoHistoryTexel alone = aoTemporalAt(320, 180, frame, reprojection, settings, depthImage(depths), Vec4{0.3f, 0.2f, 0.1f, 0.7f}, historyImage(history), false);
        assert(alone.age == 1.0f && alone.light.x == 0.3f && alone.light.w == 0.7f);
    }

    /*
    * The camera steps sideways: each texel's history held the wall's x where last frame saw it, so a texel now reads the
    * x of its own point (bilinear taps of a line, exact), in its full age. A new frame of nothing then leaves 15/16 of it.
    */
    void testMovingCameraReadsShiftedHistory() {
        const Camera previous = wallCamera(Vec3{});
        const Camera current = wallCamera(Vec3{0.1f, 0.0f, 0.0f});
        const std::vector<float> depths = wallDepths(current);
        const std::vector<float> previousDepths = wallDepths(previous);
        const AoFrame previousFrame = wallFrame(previous);
        const AoFrame frame = wallFrame(current);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        HistoryTexels history(screenSize.width * screenSize.height);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const Vec2 pixel{x + 0.5f, y + 0.5f};
                const float depth = previousDepths[y * screenSize.width + x];
                const float worldX = positionAtPixel(previousFrame.inverseViewProjection, pixel, depth, size).x;
                history[y * screenSize.width + x] =
                    AoHistoryTexel{Vec4{worldX, 0.0f, 0.0f, 1.0f}, viewDistanceAtPixel(previousFrame.inverseViewProjection, pixel, depth, size), 16.0f};
            }
        }

        const AmbientOcclusionSettings settings;
        const Mat4 reprojection = reprojectionBetween(previous, current);

        for (const Vec2 pixel : {Vec2{320.0f, 180.0f}, Vec2{100.0f, 50.0f}, Vec2{500.0f, 300.0f}}) {
            const int x = static_cast<int>(pixel.x);
            const int y = static_cast<int>(pixel.y);
            const AoHistoryTexel texel = aoTemporalAt(x, y, frame, reprojection, settings, depthImage(depths), Vec4{}, historyImage(history), true);
            const float worldX = positionAtPixel(frame.inverseViewProjection, pixel + Vec2{0.5f, 0.5f}, depths[y * screenSize.width + x], size).x + 0.1f;
            assert(near(texel.age, 16.0f, 1e-4f));
            assert(near(texel.light.x, worldX * 15.0f / 16.0f, 1e-3f));
            assert(near(texel.light.w, 15.0f / 16.0f, 1e-5f));
        }
    }

    /*
    * A block stood in front of the wall last frame and is gone now: the wall behind it is newly seen, its history held
    * the block, so it starts again from this frame alone; the wall well beside the block keeps its history, and at the
    * block's edge only some taps count, so the age is lower there. The sky keeps nothing.
    */
    void testDisocclusionStartsAgain() {
        const Camera camera = wallCamera(Vec3{});
        const std::vector<float> depths = wallDepths(camera);
        const std::vector<float> previousDepths = wallDepths(camera, -3.0f, 0.5f);
        const AoFrame frame = wallFrame(camera);
        const Vec2 size{static_cast<float>(screenSize.width), static_cast<float>(screenSize.height)};
        HistoryTexels history(screenSize.width * screenSize.height);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const float depth = previousDepths[y * screenSize.width + x];
                history[y * screenSize.width + x] =
                    AoHistoryTexel{Vec4{0.0f, 0.0f, 0.0f, 0.2f}, viewDistanceAtPixel(frame.inverseViewProjection, Vec2{x + 0.5f, y + 0.5f}, depth, size), 16.0f};
            }
        }

        const AmbientOcclusionSettings settings;
        const Vec4 current{0.5f, 0.5f, 0.5f, 1.0f};
        const AoHistoryTexel revealed = aoTemporalAt(320, 180, frame, Mat4::identity(), settings, depthImage(depths), current, historyImage(history), true);
        assert(revealed.age == 1.0f && revealed.light.x == 0.5f && revealed.light.w == 1.0f);

        const AoHistoryTexel beside = aoTemporalAt(40, 180, frame, Mat4::identity(), settings, depthImage(depths), current, historyImage(history), true);
        assert(beside.age == 16.0f && near(beside.light.w, 0.2f + 0.8f / 16.0f, 1e-5f));

        // An image with no depth at all, as the sky leaves it: nothing kept, the new frame alone.
        const HistoryTexels skyHistory(screenSize.width * screenSize.height);
        assert(aoTemporalAt(40, 180, frame, Mat4::identity(), settings, depthImage(depths), current, historyImage(skyHistory), true).age == 1.0f);

        // A sky pixel holds open, nothing gathered, no depth and no age.
        std::vector<float> withSky = depths;
        withSky[180 * screenSize.width + 320] = gpuFarDepth;
        const AoHistoryTexel sky = aoTemporalAt(320, 180, frame, Mat4::identity(), settings, depthImage(withSky), current, historyImage(history), true);
        assert(sky.age == 0.0f && sky.viewDepth == 0.0f && sky.light.x == 0.0f && sky.light.w == 1.0f);
    }

    // Half the taps showing the point give half the age, the light theirs alone.
    void testPartialHistory() {
        HistoryTexels history(screenSize.width * screenSize.height);
        history[10 * screenSize.width + 10] = AoHistoryTexel{Vec4{1.0f, 0.0f, 0.0f, 1.0f}, 5.0f, 8.0f};
        history[10 * screenSize.width + 11] = AoHistoryTexel{Vec4{1.0f, 0.0f, 0.0f, 1.0f}, 5.0f, 8.0f};
        history[11 * screenSize.width + 10] = AoHistoryTexel{Vec4{0.0f, 9.0f, 0.0f, 1.0f}, 2.0f, 8.0f};
        history[11 * screenSize.width + 11] = AoHistoryTexel{Vec4{0.0f, 9.0f, 0.0f, 1.0f}, 2.0f, 8.0f};
        const AoPreviousPlace place{Vec2{11.0f, 11.0f}, 5.0f, true};
        const AoHistory read = readAoHistory(historyImage(history), screenSize, place, 0.05f);
        assert(near(read.age, 4.0f, 1e-5f) && near(read.light.x, 1.0f, 1e-6f) && read.light.y == 0.0f);

        // Taps off the screen do not count: the corner texel alone, a quarter of its age.
        history[0] = AoHistoryTexel{Vec4{0.0f, 0.0f, 1.0f, 1.0f}, 5.0f, 8.0f};
        const AoHistory corner = readAoHistory(historyImage(history), screenSize, AoPreviousPlace{Vec2{0.0f, 0.0f}, 5.0f, true}, 0.05f);
        assert(near(corner.age, 2.0f, 1e-5f) && near(corner.light.z, 1.0f, 1e-6f));

        assert(readAoHistory(historyImage(history), screenSize, AoPreviousPlace{}, 0.05f).age == 0.0f);
    }

    // The history carries over only to a frame of the same size, gathering the same way, with last frame's camera known.
    void testHistoryUsable() {
        const AoHistoryKey key{RenderSize{640, 360}, true};
        assert(aoHistoryUsable(key, key, true));
        assert(!aoHistoryUsable(std::nullopt, key, true));
        assert(!aoHistoryUsable(key, key, false));
        assert(!aoHistoryUsable(AoHistoryKey{RenderSize{640, 361}, true}, key, true));
        assert(!aoHistoryUsable(AoHistoryKey{RenderSize{640, 360}, false}, key, true));
    }

    void testPackPass() {
        const Mat4 inverse = Mat4::translation(1.0f, 2.0f, 3.0f);
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        AmbientOcclusionSettings settings;
        settings.slices = 2;
        settings.steps = 5;
        const Mat4 reprojection = Mat4::translation(4.0f, 5.0f, 6.0f);
        const GpuAmbientOcclusionPass pass = packAmbientOcclusionPass(inverse, viewProjection, reprojection, Vec2{0.25f, -0.5f}, RenderSize{200, 100}, settings, 7, false,
                                                                      true);
        assert(pass.inverseViewProjection[12] == 1.0f && pass.inverseViewProjection[13] == 2.0f);
        assert(pass.size[0] == 200.0f && pass.size[1] == 100.0f && pass.size[2] == 1.0f / 200.0f && pass.size[3] == 1.0f / 100.0f);
        assert(near(pass.lens[0], pixelFootprint(viewProjection, 100.0f), 1e-9f));
        assert(pass.options[0] == 0.5f && pass.options[1] == 0.6f && pass.options[2] == 2.0f && pass.options[3] == 5.0f);
        assert(pass.frame[0] == 7);
        assert(pass.reprojection[12] == 4.0f && pass.lens[1] == 0.25f && pass.lens[2] == -0.5f);
        assert(pass.gather[0] == 0.25f && pass.gather[1] == 0.0f && pass.gather[2] == 8.0f);
        assert(pass.history[0] == 16.0f && pass.history[1] == 0.05f && pass.history[2] == 1.0f && pass.history[3] == 0.0f);

        // Gathering reaches further than occlusion alone.
        const GpuAmbientOcclusionPass gathering = packAmbientOcclusionPass(inverse, viewProjection, reprojection, Vec2{}, RenderSize{200, 100}, settings, 7, true, false);
        assert(gathering.options[0] == 2.0f && gathering.gather[1] == 1.0f && gathering.history[2] == 0.0f);
    }
}

void testAmbientOcclusion() {
    testSettings();
    testOccluderWeight();
    testSliceClosedForm();
    testOpenSurfaceIsFullyVisible();
    testHalfBlocked();
    testSectorShare();
    testSectorMask();
    testGatheredSampleLight();
    testDepthNormal();
    testScreenSteps();
    testMultipleBounces();
    testSpecularOcclusion();
    testSurfaceVisibility();
    testBlurWeight();
    testNoisesIndependent();
    testFloorAndWall();
    testThicknessLetsLightBehind();
    testHistoryAge();
    testHistoryTapKept();
    testPreviousPlace();
    testAccumulateAo();
    testStillCameraConverges();
    testFurnaceAndNoHistory();
    testMovingCameraReadsShiftedHistory();
    testDisocclusionStartsAgain();
    testPartialHistory();
    testHistoryUsable();
    testPackPass();
}
