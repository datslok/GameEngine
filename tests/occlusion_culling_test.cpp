#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/occlusion_culling.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/taa.h"
#include "render/render_stats.h"
#include "engine/debug_views.h"
#include "engine/settings_file.h"
#include "render/gpu/render_settings.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
    constexpr RenderSize screen{320, 180};

    // A camera at the origin looking down -Z, as the renderer's camera-relative view sees it, in the GPU's depth range.
    OcclusionView testView(const Mat4& view = Mat4::identity()) {
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(std::numbers::pi_v<float> / 3.0f, 16.0f / 9.0f, 0.1f, 2000.0f));
        return makeOcclusionView(view, projection, 0.1f, screen);
    }

    // Where a camera-relative point lands: its pixel (x, y) and its GPU depth.
    Vec3 screenPoint(const OcclusionView& view, const Mat4& projection, const Vec3& relative) {
        const Vec4 clip = projection * (view.view * Vec4{relative.x, relative.y, relative.z, 1.0f});
        const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, Vec2{static_cast<float>(screen.width), static_cast<float>(screen.height)});
        return Vec3{pixel.x, pixel.y, clip.z / clip.w};
    }

    // Points spread over a sphere's surface (a latitude-longitude grid).
    std::vector<Vec3> surfacePoints(const Vec3& centre, float radius) {
        std::vector<Vec3> points;

        for (int a = 0; a <= 24; ++a) {
            const float polar = std::numbers::pi_v<float> * static_cast<float>(a) / 24.0f;

            for (int b = 0; b < 48; ++b) {
                const float around = 2.0f * std::numbers::pi_v<float> * static_cast<float>(b) / 48.0f;
                points.push_back(centre + Vec3{std::sin(polar) * std::cos(around), std::cos(polar), std::sin(polar) * std::sin(around)} * radius);
            }
        }

        return points;
    }

    /*
    * The rectangle holds every pixel the sphere covers, and the sphere's nearest depth is at least as near as any
    * point of it (reversed-Z: larger is nearer). Spheres all over the view, turned cameras too, near and far.
    */
    void testSphereRectangleHoldsTheSphere() {
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(std::numbers::pi_v<float> / 3.0f, 16.0f / 9.0f, 0.1f, 2000.0f));
        std::mt19937 random{7};
        std::uniform_real_distribution<float> unit{-1.0f, 1.0f};
        int projected = 0;

        for (int turn = 0; turn < 3; ++turn) {
            const float yaw = 0.7f * static_cast<float>(turn);
            const float pitch = 0.3f * static_cast<float>(turn);
            const Mat4 view = Mat4::rotationY(yaw) * Mat4::rotationX(pitch);
            const OcclusionView occlusionView = makeOcclusionView(view, projection, 0.1f, screen);
            const Mat4 inverseView = Mat4::rotationX(-pitch) * Mat4::rotationY(-yaw);

            for (int i = 0; i < 400; ++i) {
                const float distance = 1.0f + 300.0f * std::abs(unit(random));
                const Vec4 inView{unit(random) * distance * 0.6f, unit(random) * distance * 0.4f, -distance, 1.0f};
                const Vec4 relative = inverseView * inView;
                const Vec3 centre{relative.x, relative.y, relative.z};
                const float radius = 0.05f + std::abs(unit(random)) * distance * 0.3f;
                const std::optional<PixelRect> rect = sphereRect(occlusionView, centre, radius);

                if (!rect) {
                    // Only spheres reaching the near plane are refused.
                    assert(distance - radius <= 0.1f + 1e-3f);
                    continue;
                }

                ++projected;
                const float nearest = sphereNearestDepth(occlusionView, centre, radius);

                for (const Vec3& point : surfacePoints(centre, radius)) {
                    const Vec3 onScreen = screenPoint(occlusionView, projection, point);
                    assert(onScreen.z <= nearest + 1e-6f);

                    const int pixelX = static_cast<int>(std::floor(onScreen.x));
                    const int pixelY = static_cast<int>(std::floor(onScreen.y));

                    if (pixelX >= 0 && pixelY >= 0 && pixelX < static_cast<int>(screen.width) && pixelY < static_cast<int>(screen.height)) {
                        assert(pixelX >= rect->minX && pixelX <= rect->maxX);
                        assert(pixelY >= rect->minY && pixelY <= rect->maxY);
                    }
                }
            }
        }

        assert(projected > 600);
    }

    // A sphere touching the near plane, or behind the camera, cannot be judged: never hidden.
    void testSphereAtTheNearPlaneIsNotProjected() {
        const OcclusionView view = testView();
        assert(!sphereRect(view, Vec3{0.0f, 0.0f, -1.0f}, 0.95f));
        assert(!sphereRect(view, Vec3{0.0f, 0.0f, 5.0f}, 1.0f));
        assert(sphereRect(view, Vec3{0.0f, 0.0f, -10.0f}, 1.0f));
    }

    // A rectangle off the screen keeps no pixels; one partly on it is clamped to the screen.
    void testRectangleClampedToTheScreen() {
        const OcclusionView view = testView();
        const std::optional<PixelRect> left = sphereRect(view, Vec3{-100.0f, 0.0f, -10.0f}, 1.0f);
        assert(left && left->empty());

        const std::optional<PixelRect> edge = sphereRect(view, Vec3{0.0f, 0.0f, -2.0f}, 1.5f);
        assert(edge && !edge->empty());
        assert(edge->minX == 0 && edge->minY == 0);
        assert(edge->maxX == static_cast<int>(screen.width) - 1 && edge->maxY == static_cast<int>(screen.height) - 1);
    }

    // The level chosen makes the rectangle touch at most 2 x 2 of its texels.
    void testLevelCoversTheRectangleWithFourTexels() {
        const std::uint32_t levels = depthPyramidLevels(screen);

        for (int minX = 0; minX < 40; minX += 3) {
            for (int width = 1; width < 200; width += 7) {
                const PixelRect rect{minX, 5, minX + width - 1, 5 + width / 2};
                const std::uint32_t level = occlusionLevel(rect, levels);
                assert(level < levels);
                assert((rect.maxX >> level) - (rect.minX >> level) <= 1);
                assert((rect.maxY >> level) - (rect.minY >> level) <= 1);
                assert(level == 0 || (rect.maxX >> (level - 1)) - (rect.minX >> (level - 1)) >= 1 ||
                       (rect.maxY >> (level - 1)) - (rect.minY >> (level - 1)) >= 1);
            }
        }
    }

    // Reversed-Z: the farther of two depths is the smaller.
    void testFartherDepth() {
        assert(fartherDepth(0.2f, 0.7f) == 0.2f);
        assert(fartherDepth(0.7f, 0.2f) == 0.2f);
        assert(nearerDepth(0.2f, 0.7f) == 0.7f);
    }

    // A depth image of random blocks at random depths, with sky (far) between them.
    std::vector<float> blockyDepth(std::mt19937& random) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        std::vector<float> depth(static_cast<std::size_t>(screen.width) * screen.height, gpuFarDepth);

        for (int block = 0; block < 30; ++block) {
            const int x0 = static_cast<int>(unit(random) * screen.width);
            const int y0 = static_cast<int>(unit(random) * screen.height);
            const int x1 = std::min(static_cast<int>(screen.width), x0 + 5 + static_cast<int>(unit(random) * 150.0f));
            const int y1 = std::min(static_cast<int>(screen.height), y0 + 5 + static_cast<int>(unit(random) * 100.0f));
            const float value = 0.1f + 0.9f * unit(random);

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    depth[static_cast<std::size_t>(y) * screen.width + static_cast<std::size_t>(x)] = value;
                }
            }
        }

        return depth;
    }

    /*
    * Conservative: whenever the pyramid says hidden, every pixel of the rectangle holds something nearer than the
    * object's nearest depth. And it does hide things: a wall in front hides what is behind it.
    */
    void testPyramidNeverHidesWhatShows() {
        std::mt19937 random{11};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        int hidden = 0;

        for (int image = 0; image < 20; ++image) {
            const std::vector<float> depth = blockyDepth(random);
            const DepthPyramid pyramid = buildDepthPyramid(depth, screen, DepthReduction::Farthest);

            for (int i = 0; i < 500; ++i) {
                const int minX = static_cast<int>(unit(random) * screen.width);
                const int minY = static_cast<int>(unit(random) * screen.height);
                const PixelRect rect{minX, minY, std::min(static_cast<int>(screen.width) - 1, minX + static_cast<int>(unit(random) * 80.0f)),
                                     std::min(static_cast<int>(screen.height) - 1, minY + static_cast<int>(unit(random) * 60.0f))};
                const float nearest = unit(random);

                if (!hiddenInPyramid(pyramid, rect, nearest)) {
                    continue;
                }

                ++hidden;

                for (int y = rect.minY; y <= rect.maxY; ++y) {
                    for (int x = rect.minX; x <= rect.maxX; ++x) {
                        assert(depth[static_cast<std::size_t>(y) * screen.width + static_cast<std::size_t>(x)] > nearest);
                    }
                }
            }
        }

        assert(hidden > 100);

        const std::vector<float> wall(static_cast<std::size_t>(screen.width) * screen.height, 0.9f);
        const DepthPyramid walled = buildDepthPyramid(wall, screen, DepthReduction::Farthest);
        assert(hiddenInPyramid(walled, PixelRect{10, 10, 200, 150}, 0.5f));
        assert(!hiddenInPyramid(walled, PixelRect{10, 10, 200, 150}, 0.95f));
        assert(!hiddenInPyramid(walled, PixelRect{10, 10, 200, 150}, 0.9f)); // touching is not behind

        const std::vector<float> sky(static_cast<std::size_t>(screen.width) * screen.height, gpuFarDepth);
        assert(!hiddenInPyramid(buildDepthPyramid(sky, screen, DepthReduction::Farthest), PixelRect{0, 0, 10, 10}, 0.0001f));
    }

    // A whole sphere behind a wall, through every step: projected, its level read, hidden; in front of it, shown.
    void testSphereBehindAWall() {
        const OcclusionView view = testView();
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(std::numbers::pi_v<float> / 3.0f, 16.0f / 9.0f, 0.1f, 2000.0f));
        const float wallDepth = screenPoint(view, projection, Vec3{0.0f, 0.0f, -20.0f}).z;
        const std::vector<float> wall(static_cast<std::size_t>(screen.width) * screen.height, wallDepth);
        const DepthPyramid pyramid = buildDepthPyramid(wall, screen, DepthReduction::Farthest);

        assert(sphereHidden(view, pyramid, Vec3{1.0f, 0.5f, -40.0f}, 3.0f));
        assert(!sphereHidden(view, pyramid, Vec3{1.0f, 0.5f, -10.0f}, 3.0f));
        assert(!sphereHidden(view, pyramid, Vec3{1.0f, 0.5f, -21.0f}, 3.0f)); // pokes through the wall
        assert(!sphereHidden(view, pyramid, Vec3{0.0f, 0.0f, -40.0f}, -1.0f)); // unknown bounds
    }

    // Each batch's run of draw list entries and its mesh's triangles, for the GPU's count.
    void testPackedBatches() {
        const std::vector<DrawBatch> batches{
            DrawBatch{MeshHandle{0}, 0, 3, false, false},
            DrawBatch{MeshHandle{1}, 3, 2, true, false},
        };
        const std::vector<std::uint32_t> triangles{100, 7};
        const std::vector<GpuOcclusionBatch> packed = packOcclusionBatches(batches, [&](MeshHandle mesh) { return triangles.at(mesh.index); });

        assert(packed.size() == 2);
        assert(packed[0].firstEntry == 0 && packed[0].count == 3 && packed[0].triangles == 100);
        assert(packed[1].firstEntry == 3 && packed[1].count == 2 && packed[1].triangles == 7);
    }

    // On by default for now (the measuring stage), switchable while the game runs as passes.measureOcclusion.
    void testSetting() {
        assert(RenderSettings{}.measureOcclusion);

        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) { visit(settings, visitor); };
        const std::vector<std::string> warnings = applySettings({{"passes.measureOcclusion", "false"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(!settings.measureOcclusion);
    }

    // F4 shows what would be hidden only once it has been measured.
    void testReadoutLine() {
        RenderStats stats;
        const auto mentions = [](const std::vector<std::string>& lines) {
            for (const std::string& line : lines) {
                if (line.starts_with("HIDDEN")) {
                    return true;
                }
            }

            return false;
        };

        assert(!mentions(renderStatsLines(stats)));
        stats.occlusion = OcclusionCounts{.tested = 200, .hidden = 50, .testedTriangles = 4'000'000, .hiddenTriangles = 1'000'000};
        assert(mentions(renderStatsLines(stats)));
    }
}

/*
* Occlusion culling's tests (render/gpu/occlusion_culling.h): the sphere's rectangle on screen and nearest depth,
* the pyramid's level, the farthest-depth pyramid, and the hidden test being conservative.
*/
void testOcclusionCulling() {
    testSphereRectangleHoldsTheSphere();
    testSphereAtTheNearPlaneIsNotProjected();
    testRectangleClampedToTheScreen();
    testLevelCoversTheRectangleWithFourTexels();
    testFartherDepth();
    testPyramidNeverHidesWhatShows();
    testSphereBehindAWall();
    testPackedBatches();
    testReadoutLine();
    testSetting();
}
