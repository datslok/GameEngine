#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"
#include "scene/camera.h"

#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    const Vec2 screen{160.0f, 90.0f};
    constexpr RenderSize screenSize{160, 90};

    // Standing 1.5 m above a floor at y = 0, looking down -z, a wall facing the camera at z = -10.
    Camera makeCamera() {
        return Camera{Vec3{0.0f, 1.5f, 0.0f}, Vec3{0.0f, 1.5f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, screen.x / screen.y, 0.1f, 500.0f};
    }

    struct Scene {
        bool wall = true;
        // A floating panel facing the camera at z = -6, 0.6 wide and 0.4 high round the camera's eye height: it hides the
        // floor's reflected ray from the camera where the ray passes 2 m behind it.
        bool panel = false;
    };

    // The nearest of the scene's surfaces along a ray from the camera (measured from it): its distance, or none.
    std::optional<float> nearestSurface(const Vec3& direction, const Scene& scene) {
        const Vec3 eye{0.0f, 1.5f, 0.0f};
        std::optional<float> nearest;
        const auto keep = [&](float distance) {
            if (distance > 0.0f && (!nearest || distance < *nearest)) {
                nearest = distance;
            }
        };

        if (direction.y < 0.0f) {
            keep(-eye.y / direction.y);
        }

        if (direction.z < 0.0f) {
            if (scene.wall) {
                keep(-10.0f / direction.z);
            }

            if (scene.panel) {
                const float distance = -6.0f / direction.z;
                const Vec3 point = eye + direction * distance;

                if (std::abs(point.x) <= 0.3f && point.y >= 1.3f && point.y <= 1.7f) {
                    keep(distance);
                }
            }
        }

        return nearest;
    }

    // The depth image the scene draws: each pixel's centre through the camera, its nearest surface's depth, or the far depth.
    std::vector<float> drawDepths(const Mat4& viewProjection, const Mat4& inverseViewProjection, const Scene& scene) {
        std::vector<float> depths(screenSize.width * screenSize.height, gpuFarDepth);

        for (std::uint32_t y = 0; y < screenSize.height; ++y) {
            for (std::uint32_t x = 0; x < screenSize.width; ++x) {
                const Vec2 pixel{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                const Vec3 direction = positionAtPixel(inverseViewProjection, pixel, 0.5f, screen).normalized();

                if (const std::optional<float> distance = nearestSurface(direction, scene)) {
                    const Vec3 point = direction * *distance;
                    const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
                    depths[y * screenSize.width + x] = clip.z / clip.w;
                }
            }
        }

        return depths;
    }

    // Each level a full mip chain, rounded down, never below 1, and capped.
    void testPyramidSizes() {
        assert(depthPyramidLevels(RenderSize{1920, 1080}) == 11);
        assert(depthPyramidLevels(RenderSize{1, 1}) == 1);
        assert(depthPyramidLevels(RenderSize{65536, 4}) == SSR_MAX_PYRAMID_LEVELS);

        assert((pyramidLevelSize(RenderSize{5, 3}, 0) == RenderSize{5, 3}));
        assert((pyramidLevelSize(RenderSize{5, 3}, 1) == RenderSize{2, 1}));
        assert((pyramidLevelSize(RenderSize{5, 3}, 2) == RenderSize{1, 1}));
        assert((pyramidLevelSize(RenderSize{5, 3}, 5) == RenderSize{1, 1}));
    }

    // The far edge of an odd level is folded into the last cell above it, at every level, so no texel is left out.
    void testPyramidCells() {
        assert(pyramidLastFine(0, 2, 5) == 1);
        assert(pyramidLastFine(1, 2, 5) == 4);
        assert(pyramidLastFine(1, 2, 4) == 3);
        assert(pyramidLastFine(0, 1, 1) == 0);
        assert(pyramidLastFine(0, 1, 3) == 2);

        assert(pyramidCell(1, 1, 2) == 0);
        assert(pyramidCell(3, 1, 2) == 1);
        assert(pyramidCell(4, 1, 2) == 1);
        assert(pyramidCell(4, 0, 5) == 4);
        assert(pyramidCell(7, 2, 1) == 0);

        assert(nearerDepth(0.2f, 0.7f) == 0.7f && nearerDepth(0.7f, 0.2f) == 0.7f);
    }

    /*
    * Every texel of the pyramid is the nearest of the depth image's texels that fall in its cell: no texel nearer than
    * its cell, and one of them exactly as near.
    */
    void testDepthPyramid() {
        for (const RenderSize size : {RenderSize{5, 3}, RenderSize{8, 8}, RenderSize{13, 6}, RenderSize{1, 7}}) {
            std::vector<float> depths(size.width * size.height);

            for (std::size_t i = 0; i < depths.size(); ++i) {
                depths[i] = std::fmod(static_cast<float>(i) * 0.618034f, 1.0f);
            }

            const DepthPyramid pyramid = buildDepthPyramid(depths, size);
            assert(pyramid.levels.size() == depthPyramidLevels(size));

            for (std::uint32_t level = 0; level < pyramid.levels.size(); ++level) {
                const RenderSize levelSize = pyramid.sizes[level];
                assert(levelSize == pyramidLevelSize(size, level));
                std::vector<float> nearest(levelSize.width * levelSize.height, -1.0f);

                for (std::uint32_t y = 0; y < size.height; ++y) {
                    for (std::uint32_t x = 0; x < size.width; ++x) {
                        const int cellX = pyramidCell(static_cast<int>(x), level, static_cast<int>(levelSize.width));
                        const int cellY = pyramidCell(static_cast<int>(y), level, static_cast<int>(levelSize.height));
                        float& cell = nearest[static_cast<std::size_t>(cellY) * levelSize.width + static_cast<std::size_t>(cellX)];
                        cell = std::max(cell, depths[y * size.width + x]);
                    }
                }

                for (std::uint32_t y = 0; y < levelSize.height; ++y) {
                    for (std::uint32_t x = 0; x < levelSize.width; ++x) {
                        assert(pyramid.nearest(level, static_cast<int>(x), static_cast<int>(y)) == nearest[y * levelSize.width + x]);
                    }
                }
            }
        }
    }

    // A pixel and its depth back to the point that drew it, and its distance along the view.
    void testPositionAtPixel() {
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        const Mat4 inverseViewProjection = *inverse(viewProjection);

        for (const Vec3 point : {Vec3{0.0f, -1.5f, -4.0f}, Vec3{2.0f, 0.5f, -10.0f}, Vec3{-0.3f, 0.1f, -0.5f}}) {
            const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
            const Vec2 pixel{(clip.x / clip.w + 1.0f) * 0.5f * screen.x, (1.0f - clip.y / clip.w) * 0.5f * screen.y};
            const Vec3 back = positionAtPixel(inverseViewProjection, pixel, clip.z / clip.w, screen);
            assert(near(back.x, point.x, 1e-3f) && near(back.y, point.y, 1e-3f) && near(back.z, point.z, 1e-3f));
            assert(near(viewDistanceAtPixel(inverseViewProjection, pixel, clip.z / clip.w, screen), clip.w, 1e-3f));
        }

        // One pixel at 1 m covers the screen's height at 1 m, split into its pixels.
        const Camera camera = makeCamera();
        assert(near(pixelFootprint(viewProjection, screen.y), 2.0f * std::tan(0.5f) / screen.y, 1e-6f));
        assert(near(pixelFootprint(cameraRelativeViewProjection(camera), 1000.0f), 2.0f * std::tan(0.5f) / 1000.0f, 1e-7f));
    }

    // Is the screen point at t a point of the line in space? Unproject it and measure its distance from the line.
    float distanceFromLine(const ScreenRay& ray, float t, const Mat4& inverseViewProjection, const Vec3& origin, const Vec3& direction) {
        const Vec3 onScreen = ray.start + ray.step * t;
        const Vec3 point = positionAtPixel(inverseViewProjection, Vec2{onScreen.x, onScreen.y}, onScreen.z, screen);
        const Vec3 offset = point - origin;
        return (offset - direction * offset.dot(direction)).length();
    }

    /*
    * The ray on screen is the line in space: every point along it unprojects onto the line, it starts at the point's
    * pixel and ends at the screen's edge, the near plane or its reach, whichever comes first.
    */
    void testScreenRay() {
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const Vec3 floorPoint{0.0f, -1.5f, -4.0f};

        // Up and away, reflected off the floor: it leaves the top of the screen or reaches the far distance.
        const Vec3 view = floorPoint.normalized();
        const Vec3 up{view.x, -view.y, view.z};
        const ScreenRay away = screenRay(viewProjection, floorPoint, up, 100.0f, screen);
        const Vec4 clip = viewProjection * Vec4{floorPoint.x, floorPoint.y, floorPoint.z, 1.0f};
        assert(near(away.start.z, clip.z / clip.w, 1e-6f));
        assert(away.end > 0.0f && away.end <= 1.0f);

        for (float share : {0.0f, 0.25f, 0.5f, 1.0f}) {
            assert(distanceFromLine(away, share * away.end, inverseViewProjection, floorPoint, up) < 1e-3f * (1.0f + 100.0f * share));
            const Vec3 point = away.start + away.step * (share * away.end);
            assert(point.x >= -1e-3f && point.x <= screen.x + 1e-3f && point.y >= -1e-3f && point.y <= screen.y + 1e-3f);
            assert(point.z >= -1e-6f && point.z <= 1.0f + 1e-6f);
        }

        // Sideways: cut at the screen's right edge.
        const ScreenRay sideways = screenRay(viewProjection, floorPoint, Vec3{1.0f, 0.0f, 0.0f}, 100.0f, screen);
        assert(near(sideways.start.x + sideways.step.x * sideways.end, screen.x, 1e-2f));

        // Towards the camera: cut at the near plane (depth 1), before it can pass the camera.
        const ScreenRay back = screenRay(viewProjection, floorPoint, Vec3{0.0f, 0.0f, 1.0f}, 100.0f, screen);
        const Vec3 last = back.start + back.step * back.end;
        assert(last.z <= 1.0f + 1e-5f && back.end > 0.0f);
        assert(distanceFromLine(back, 0.5f * back.end, inverseViewProjection, floorPoint, Vec3{0.0f, 0.0f, 1.0f}) < 1e-3f);

        // A short reach ends where it does: 1 m along.
        const ScreenRay shortRay = screenRay(viewProjection, floorPoint, Vec3{0.0f, 0.0f, -1.0f}, 1.0f, screen);
        const Vec3 end = shortRay.start + shortRay.step * shortRay.end;
        const Vec3 endPoint = positionAtPixel(inverseViewProjection, Vec2{end.x, end.y}, end.z, screen);
        assert(near(endPoint.z, -5.0f, 1e-3f));
    }

    // Out of the cell along the ray, a hair into the next, in either direction; not moving on screen, never out.
    void testCellExit() {
        assert(near(cellExit(Vec2{0.5f, 0.5f}, Vec2{1.0f, 0.0f}, 1.0f), 0.5f + SSR_CELL_CROSSING, 1e-6f));
        assert(near(cellExit(Vec2{0.5f, 0.5f}, Vec2{-2.0f, 0.0f}, 1.0f), (0.5f + SSR_CELL_CROSSING) / 2.0f, 1e-6f));
        assert(near(cellExit(Vec2{5.0f, 1.0f}, Vec2{0.0f, -1.0f}, 4.0f), 1.0f + SSR_CELL_CROSSING, 1e-6f));
        // Whichever side it reaches first.
        assert(near(cellExit(Vec2{1.0f, 3.5f}, Vec2{1.0f, 1.0f}, 4.0f), 0.5f + SSR_CELL_CROSSING, 1e-6f));
        // On an edge going back, it still moves into the cell behind.
        assert(near(cellExit(Vec2{4.0f, 1.0f}, Vec2{-1.0f, 0.0f}, 4.0f), SSR_CELL_CROSSING, 1e-6f));
        assert(cellExit(Vec2{1.0f, 1.0f}, Vec2{0.0f, 0.0f}, 1.0f) > 1e20f);
    }

    // The four moves of the march.
    void testHizStep() {
        // A ray going deeper by 0.1 a unit of t and 1 texel right.
        const ScreenRay ray{Vec3{0.5f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, -0.1f}, 100.0f};

        // In front of the cell's nearest depth, which it reaches inside the cell: on to it, a level finer.
        const HizTrace reaches = hizStep(HizTrace{ray.start, 0.0f, 2}, ray, 0.45f, 5);
        assert(near(reaches.t, 0.5f, 1e-5f) && near(reaches.position.z, 0.45f, 1e-6f) && reaches.level == 1);

        // In front, not reaching it before the cell's edge: out of the cell, a level coarser.
        const HizTrace crosses = hizStep(HizTrace{ray.start, 0.0f, 1}, ray, 0.1f, 5);
        assert(near(crosses.t, 1.5f + SSR_CELL_CROSSING, 1e-5f) && crosses.level == 2);
        assert(near(crosses.position.x, 2.0f + SSR_CELL_CROSSING, 1e-5f));

        // At or behind the cell's nearest: a level finer, not moving; at level 0, a hit.
        const HizTrace behind = hizStep(HizTrace{ray.start, 0.0f, 3}, ray, 0.6f, 5);
        assert(behind.level == 2 && behind.t == 0.0f);
        assert(hizStep(HizTrace{ray.start, 0.0f, 0}, ray, 0.5f, 5).level < 0);

        // Coming towards the camera, it never reaches a nearer depth: across the cell, but never above the top level.
        const ScreenRay towards{Vec3{0.5f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.1f}, 100.0f};
        assert(hizStep(HizTrace{towards.start, 0.0f, 0}, towards, 0.4f, 5).level == 1);
        assert(hizStep(HizTrace{towards.start, 0.0f, 5}, towards, 0.4f, 5).level == 5);

        // Passing behind a surface too thick to be it: on out of the texel, looking at level 0.
        const HizTrace passed = passTexel(HizTrace{ray.start, 0.0f, -1}, ray);
        assert(passed.level == 0 && near(passed.t, 0.5f + SSR_CELL_CROSSING, 1e-5f));

        assert(withinThickness(10.2f, 10.0f, 0.3f));
        assert(withinThickness(9.9f, 10.0f, 0.3f));
        assert(!withinThickness(10.4f, 10.0f, 0.3f));
    }

    struct Traced {
        std::optional<ScreenHit> hit;
        Vec3 point; // the hit, measured from the camera
    };

    // A floor point's reflection, traced through the scene's depth image.
    Traced traceFloorReflection(const Scene& scene, float thickness) {
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const std::vector<float> depths = drawDepths(viewProjection, inverseViewProjection, scene);
        const DepthPyramid pyramid = buildDepthPyramid(depths, screenSize);

        // The pixel showing the floor 4 m ahead, as the depth image drew it.
        const Vec4 clip = viewProjection * Vec4{0.0f, -1.5f, -4.0f, 1.0f};
        const Vec2 pixel{std::floor((clip.x / clip.w + 1.0f) * 0.5f * screen.x) + 0.5f, std::floor((1.0f - clip.y / clip.w) * 0.5f * screen.y) + 0.5f};
        const float depth = depths[static_cast<std::size_t>(pixel.y) * screenSize.width + static_cast<std::size_t>(pixel.x)];
        const Vec3 position = positionAtPixel(inverseViewProjection, pixel, depth, screen);

        const Vec3 view = position.normalized();
        const Vec3 reflected{view.x, -view.y, view.z};
        const float offset = SSR_START_OFFSET_PIXELS * pixelFootprint(viewProjection, screen.y) * viewDistanceAtPixel(inverseViewProjection, pixel, depth, screen);
        const ScreenRay ray = screenRay(viewProjection, position + Vec3{0.0f, offset, 0.0f}, reflected, 100.0f, screen);

        Traced traced;
        traced.hit = traceScreenRay(pyramid, ray, inverseViewProjection, thickness, 256);

        // What the hit pixel shows: the surface in the depth image there (the ray itself may be a little behind it).
        if (traced.hit) {
            const Vec2 hitPixel{std::floor(traced.hit->position.x) + 0.5f, std::floor(traced.hit->position.y) + 0.5f};
            const float surface = depths[static_cast<std::size_t>(hitPixel.y) * screenSize.width + static_cast<std::size_t>(hitPixel.x)];
            traced.point = positionAtPixel(inverseViewProjection, hitPixel, surface, screen);
        }

        return traced;
    }

    /*
    * The floor 4 m ahead of an eye 1.5 m up reflects the wall 10 m away at 1.5 x 6 / 4 = 2.25 m up (the mirror image of
    * the eye, seen through the floor). Without the wall the ray leaves into the sky and misses. A panel the ray passes
    * 2 m behind is skipped with a thin thickness, and wrongly hit with a thick one.
    */
    void testTraceScreenRay() {
        const Traced wall = traceFloorReflection(Scene{}, 0.3f);
        assert(wall.hit);
        assert(near(wall.point.z, -10.0f, 0.15f));
        assert(near(wall.point.y + 1.5f, 2.25f, 0.15f));

        assert(!traceFloorReflection(Scene{.wall = false}, 0.3f).hit);

        const Traced past = traceFloorReflection(Scene{.wall = true, .panel = true}, 0.3f);
        assert(past.hit && near(past.point.z, -10.0f, 0.15f));

        const Traced caught = traceFloorReflection(Scene{.wall = true, .panel = true}, 5.0f);
        assert(caught.hit && near(caught.point.z, -6.0f, 0.15f));
    }

    // Each fade alone: roughness, the screen's edges, the reach's end, and hits from behind.
    void testConfidence() {
        const ReflectionSettings settings;
        const Vec2 centre{80.0f, 45.0f};
        const auto confidence = [&](float roughness, Vec2 pixel, float travelled, float facing) {
            return reflectionConfidence(ReflectionHit{roughness, pixel, travelled, facing}, screen, settings);
        };

        assert(confidence(0.1f, centre, 5.0f, -1.0f) == 1.0f);
        assert(confidence(settings.sharpRoughness, centre, 5.0f, -1.0f) == 1.0f);
        assert(confidence(settings.maxRoughness, centre, 5.0f, -1.0f) == 0.0f);
        const float between = confidence(0.5f * (settings.sharpRoughness + settings.maxRoughness), centre, 5.0f, -1.0f);
        assert(near(between, 0.5f, 1e-5f));

        assert(confidence(0.1f, Vec2{0.0f, 45.0f}, 5.0f, -1.0f) == 0.0f);
        assert(confidence(0.1f, Vec2{80.0f, 90.0f}, 5.0f, -1.0f) == 0.0f);
        assert(confidence(0.1f, Vec2{0.5f * settings.edgeFade * 80.0f, 45.0f}, 5.0f, -1.0f) > 0.0f);
        assert(confidence(0.1f, Vec2{0.5f * settings.edgeFade * 80.0f, 45.0f}, 5.0f, -1.0f) < 1.0f);

        assert(confidence(0.1f, centre, settings.reach, -1.0f) == 0.0f);
        assert(confidence(0.1f, centre, SSR_DISTANCE_FADE_START * settings.reach, -1.0f) == 1.0f);

        assert(confidence(0.1f, centre, 5.0f, 0.0f) == 0.0f);
        assert(confidence(0.1f, centre, 5.0f, 0.5f) == 0.0f);
        assert(confidence(0.1f, centre, 5.0f, -SSR_FRONT_FACE_COSINE) == 1.0f);
    }

    // A mirror reads the sharp picture; a rougher surface or a longer ray a blurrier level, one more per doubling.
    void testConeLevel() {
        assert(reflectionConeLevel(0.0f, 500.0f) == 0.0f);
        assert(reflectionConeLevel(0.05f, 10.0f) == 0.0f);

        const float level = reflectionConeLevel(0.5f, 100.0f);
        assert(near(level, std::log2(2.0f * 100.0f * static_cast<float>(SSR_CONE_SPREAD) * 0.25f), 1e-5f));
        assert(near(reflectionConeLevel(0.5f, 200.0f), level + 1.0f, 1e-5f));
        assert(reflectionConeLevel(0.6f, 100.0f) > level);
    }

    // The stand-in swapped for the reflection as far as it is trusted; untouched when not at all.
    void testReflectedScene() {
        const Vec3 scene{0.7f, 0.5f, 0.3f};
        const Vec3 standIn{0.2f, 0.2f, 0.2f};
        const Vec3 albedo{0.04f, 0.04f, 0.04f};
        const Vec3 hit{10.0f, 5.0f, 1.0f};

        const Vec3 untouched = reflectedScene(scene, standIn, albedo, hit, 0.0f);
        assert(untouched.x == scene.x && untouched.y == scene.y && untouched.z == scene.z);

        const Vec3 swapped = reflectedScene(scene, standIn, albedo, hit, 1.0f);
        assert(near(swapped.x, 0.7f - 0.2f + 0.4f, 1e-6f) && near(swapped.y, 0.5f, 1e-6f) && near(swapped.z, 0.3f - 0.2f + 0.04f, 1e-6f));

        const Vec3 half = reflectedScene(scene, standIn, albedo, hit, 0.5f);
        assert(near(half.x, 0.7f + 0.5f * 0.2f, 1e-6f));

        // A dark reflection cannot take more than the scene has.
        const Vec3 dark = reflectedScene(Vec3{0.1f, 0.1f, 0.1f}, Vec3{0.5f, 0.5f, 0.5f}, albedo, Vec3{}, 1.0f);
        assert(dark.x == 0.0f && dark.y == 0.0f && dark.z == 0.0f);
    }

    void testValidation() {
        validateReflections(ReflectionSettings{});

        const auto rejects = [](ReflectionSettings settings) {
            try {
                validateReflections(settings);
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };

        assert(rejects(ReflectionSettings{.sharpRoughness = 0.6f, .maxRoughness = 0.6f}));
        assert(rejects(ReflectionSettings{.sharpRoughness = -0.1f}));
        assert(rejects(ReflectionSettings{.maxRoughness = 1.1f}));
        assert(rejects(ReflectionSettings{.thickness = -1.0f}));
        assert(rejects(ReflectionSettings{.reach = 0.0f}));
        assert(rejects(ReflectionSettings{.maxSteps = 0}));
        assert(rejects(ReflectionSettings{.edgeFade = 1.5f}));
        assert(rejects(ReflectionSettings{.thickness = std::numeric_limits<float>::quiet_NaN()}));
        assert(rejects(ReflectionSettings{.probeSize = 100}));
        assert(rejects(ReflectionSettings{.probeSize = 8}));
        assert(rejects(ReflectionSettings{.probeFar = 0.01f}));
    }

    // The pass's numbers land where reflections.comp reads them.
    void testPackReflectionPass() {
        const Mat4 viewProjection = cameraRelativeViewProjection(makeCamera());
        const Mat4 inverseViewProjection = *inverse(viewProjection);
        const ReflectionSettings settings;
        const ReflectionPassFrame frame{.pyramidLevels = 8, .screenSpace = true, .probeLevels = 7, .probeOrigin = Vec3{1.0f, -2.0f, 3.0f},
                                        .probeSize = 256, .probeDistanceLevels = 9, .background = Vec3{0.1f, 0.2f, 0.3f}};
        const GpuReflectionPass pass = packReflectionPass(viewProjection, inverseViewProjection, Mat4::identity(), screenSize,
                                                          Vec2{0.25f, -0.125f}, settings, frame);

        // Column-major: element [column * 4 + row].
        assert(pass.viewProjection[1 * 4 + 0] == viewProjection.values[0][1]);
        assert(pass.inverseViewProjection[3 * 4 + 2] == inverseViewProjection.values[2][3]);
        assert(pass.reprojection[0] == 1.0f && pass.reprojection[1] == 0.0f);
        assert(pass.size[0] == 160.0f && pass.size[1] == 90.0f && pass.size[2] == 1.0f / 160.0f);
        assert(pass.options[0] == settings.sharpRoughness && pass.options[1] == settings.maxRoughness);
        assert(pass.options[2] == settings.thickness && pass.options[3] == settings.reach);
        assert(pass.limits[0] == static_cast<float>(settings.maxSteps) && pass.limits[1] == 7.0f && pass.limits[2] == settings.edgeFade);
        assert(pass.limits[3] == 0.0f);
        assert(pass.lens[0] == pixelFootprint(viewProjection, 90.0f) && pass.lens[1] == 0.25f && pass.lens[2] == -0.125f);
        assert(pass.probe[0] == 1.0f && pass.probe[1] == 7.0f && pass.probe[2] == 1.0f && pass.probe[3] == settings.probeFar);
        assert(pass.probeOrigin[0] == 1.0f && pass.probeOrigin[1] == -2.0f && pass.probeOrigin[2] == 3.0f);
        assert(pass.probeCube[0] == 256.0f && pass.probeCube[1] == 9.0f);
        assert(pass.background[0] == 0.1f && pass.background[1] == 0.2f && pass.background[2] == 0.3f);

        // Ray-traced reflections are a ray tracing setting: off by default, as every ray-traced feature is.
        assert(!ReflectionSettings{}.rayTraced);

        // Without a probe this frame (none captured yet, or switched off) and without last frame's picture.
        const GpuReflectionPass bare = packReflectionPass(viewProjection, inverseViewProjection, Mat4::identity(), screenSize, Vec2{}, settings,
                                                          ReflectionPassFrame{.pyramidLevels = 8, .probeOrigin = Vec3{}, .probeSize = 0, .probeDistanceLevels = 0, .background = Vec3{}});
        assert(bare.probe[0] == 0.0f && bare.probe[2] == 0.0f);

        const GpuPyramidPass pyramid = packPyramidPass(RenderSize{80, 45}, 1, RenderSize{160, 90});
        assert(pyramid.target[0] == 80 && pyramid.target[1] == 45 && pyramid.target[2] == 1);
        assert(pyramid.source[0] == 160 && pyramid.source[1] == 90);
    }
}

void testScreenReflections() {
    testPyramidSizes();
    testPyramidCells();
    testDepthPyramid();
    testPositionAtPixel();
    testScreenRay();
    testCellExit();
    testHizStep();
    testTraceScreenRay();
    testConfidence();
    testConeLevel();
    testReflectedScene();
    testValidation();
    testPackReflectionPass();
}
