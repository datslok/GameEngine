#include "math/mat4.h"
#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"
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
                sum += ambientOcclusionAt(x, y, frame, settings, (i + 0.5f) / grid, (j + 0.5f) / grid, depthAt);
            }
        }

        return static_cast<float>(sum / (grid * grid));
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

        assert(ambientOcclusionAt(static_cast<int>(screenSize.width / 2), 0, frame, settings, 0.5f, 0.5f, depthAt) == 1.0f);
    }

    void testPackPass() {
        const Mat4 inverse = Mat4::translation(1.0f, 2.0f, 3.0f);
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        AmbientOcclusionSettings settings;
        settings.slices = 2;
        settings.steps = 5;
        const GpuAmbientOcclusionPass pass = packAmbientOcclusionPass(inverse, viewProjection, RenderSize{200, 100}, settings, 7);
        assert(pass.inverseViewProjection[12] == 1.0f && pass.inverseViewProjection[13] == 2.0f);
        assert(pass.size[0] == 200.0f && pass.size[1] == 100.0f && pass.size[2] == 1.0f / 200.0f && pass.size[3] == 1.0f / 100.0f);
        assert(near(pass.lens[0], pixelFootprint(viewProjection, 100.0f), 1e-9f));
        assert(pass.options[0] == 0.5f && pass.options[1] == 0.6f && pass.options[2] == 2.0f && pass.options[3] == 5.0f);
        assert(pass.frame[0] == 7);
    }
}

void testAmbientOcclusion() {
    testSettings();
    testOccluderWeight();
    testSliceClosedForm();
    testOpenSurfaceIsFullyVisible();
    testHalfBlocked();
    testDepthNormal();
    testScreenSteps();
    testMultipleBounces();
    testSpecularOcclusion();
    testBlurWeight();
    testFloorAndWall();
    testPackPass();
}
