#include "render/gpu/clusters.h"

#include "engine/settings_file.h"
#include "math/angles.h"
#include "math/mat4.h"
#include "math/vec4.h"
#include "render/gpu/render_settings.h"
#include "scene/camera.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // The demo's lens: 70 degrees high, near 0.1 and far 100, looking somewhere other than down an axis.
    Camera tiltedCamera(float aspectRatio) {
        return Camera{Vec3{3.0f, 2.0f, -1.0f}, Vec3{-2.0f, 0.5f, -9.0f}, Vec3{0.0f, 1.0f, 0.0f}, radiansFromDegrees(70.0f), aspectRatio, 0.1f, 100.0f};
    }

    // A small repeatable generator (std::uniform_real_distribution differs between standard libraries).
    class TestRandom {
        public:
            float between(float lowest, float highest) {
                state = state * 1664525u + 1013904223u;
                const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
                return lowest + (highest - lowest) * unit;
            }

            Vec3 direction() {
                while (true) {
                    const Vec3 candidate{between(-1.0f, 1.0f), between(-1.0f, 1.0f), between(-1.0f, 1.0f)};
                    const float lengthSquared = candidate.lengthSquared();

                    if (lengthSquared > 0.01f && lengthSquared <= 1.0f) {
                        return candidate / std::sqrt(lengthSquared);
                    }
                }
            }

        private:
            std::uint32_t state = 12345u;
    };

    // A point seen at a pixel (from the top-left corner) at a depth along the view direction, measured from the camera.
    Vec3 pointAtPixel(const ClusterGrid& grid, float pixelX, float pixelY, float depth) {
        const float ndcX = 2.0f * pixelX / static_cast<float>(grid.screenWidth) - 1.0f;
        const float ndcY = 1.0f - 2.0f * pixelY / static_cast<float>(grid.screenHeight);
        return grid.right * (ndcX * depth * grid.tanHalfWidth) + grid.up * (ndcY * depth * grid.tanHalfHeight) + grid.forward * depth;
    }

    // A depth between near and far, spread evenly over the logarithm so every slice gets its share.
    float randomDepth(TestRandom& random, const ClusterGrid& grid) {
        return grid.nearDepth * std::pow(grid.farDepth / grid.nearDepth, random.between(0.0f, 1.0f));
    }

    GpuPointLight pointLight(const Vec3& position, float range) {
        GpuPointLight light{};
        light.positionRange[0] = position.x;
        light.positionRange[1] = position.y;
        light.positionRange[2] = position.z;
        light.positionRange[3] = range;
        return light;
    }

    GpuSpotLight spotLight(const Vec3& position, float range, const Vec3& direction, float cosOuter) {
        GpuSpotLight light{};
        light.positionRange[0] = position.x;
        light.positionRange[1] = position.y;
        light.positionRange[2] = position.z;
        light.positionRange[3] = range;
        light.directionCosOuter[0] = direction.x;
        light.directionCosOuter[1] = direction.y;
        light.directionCosOuter[2] = direction.z;
        light.directionCosOuter[3] = cosOuter;
        light.radianceCosInner[3] = std::min(1.0f, cosOuter + 0.05f);
        return light;
    }

    void addPoint(GpuLights& lights, const GpuPointLight& light) {
        lights.points.push_back(light);
        lights.counts[1] = static_cast<std::int32_t>(lights.points.size());
    }

    void addSpot(GpuLights& lights, const GpuSpotLight& light) {
        lights.spots.push_back(light);
        lights.counts[2] = static_cast<std::int32_t>(lights.spots.size());
    }

    // Tiles of 64 pixels, the last column and row cut off by the screen's edge: 2560 x 1440 is 40 x 23 tiles (22.5 rounded up).
    void testGridSize() {
        const Camera camera = tiltedCamera(16.0f / 9.0f);

        const ClusterGrid large = makeClusterGrid(camera, 2560, 1440);
        assert(large.tilesX == 40);
        assert(large.tilesY == 23);
        assert(large.slices == clusterDepthSlices);
        assert(clusterCount(large) == 40 * 23 * clusterDepthSlices);

        const ClusterGrid small = makeClusterGrid(camera, 100, 64);
        assert(small.tilesX == 2);
        assert(small.tilesY == 1);

        bool threw = false;

        try {
            makeClusterGrid(camera, 0, 720);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    // The grid's lens and axes are the camera's: the depth is the distance along the view, and the axes are orthonormal.
    void testGridFollowsTheCamera() {
        const Camera camera = tiltedCamera(1.5f);
        const ClusterGrid grid = makeClusterGrid(camera, 1200, 800);

        assert(nearlyEqual(grid.nearDepth, 0.1f, 1e-7f));
        assert(nearlyEqual(grid.farDepth, 100.0f, 1e-5f));
        assert(nearlyEqual(grid.tanHalfHeight, std::tan(radiansFromDegrees(35.0f)), 1e-6f));
        assert(nearlyEqual(grid.tanHalfWidth, 1.5f * grid.tanHalfHeight, 1e-6f));

        assert(nearlyEqual(grid.forward.dot(camera.getForward()), 1.0f, 1e-6f));
        assert(nearlyEqual(grid.right.length(), 1.0f, 1e-6f));
        assert(nearlyEqual(grid.up.length(), 1.0f, 1e-6f));
        assert(nearlyEqual(grid.right.dot(grid.up), 0.0f, 1e-6f));
        assert(nearlyEqual(grid.up.dot(grid.forward), 0.0f, 1e-6f));

        const Vec3 view = toClusterView(grid, camera.getForward() * 7.0f + grid.right * 2.0f - grid.up * 3.0f);
        assert(nearlyEqual(view.x, 2.0f, 1e-5f));
        assert(nearlyEqual(view.y, -3.0f, 1e-5f));
        assert(nearlyEqual(view.z, 7.0f, 1e-5f));
    }

    /*
    * Slices are spaced exponentially: slice k starts at near * (far / near)^(k / slices), so each is the same factor
    * deeper than the last (1000^(1/24) = 1.334 for near 0.1 and far 100), and a depth just past a slice's start is in it.
    * Depths a hair outside near and far are clamped into the grid.
    */
    void testSlicesAreExponential() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 1280, 720);

        assert(nearlyEqual(sliceStartDepth(0, grid.sliceScale, grid.sliceBias), 0.1f, 1e-6f));
        assert(nearlyEqual(sliceStartDepth(grid.slices, grid.sliceScale, grid.sliceBias), 100.0f, 1e-3f));

        const float factor = std::pow(1000.0f, 1.0f / static_cast<float>(clusterDepthSlices));

        for (int slice = 0; slice < grid.slices; ++slice) {
            const float start = sliceStartDepth(slice, grid.sliceScale, grid.sliceBias);
            const float end = sliceStartDepth(slice + 1, grid.sliceScale, grid.sliceBias);
            assert(nearlyEqual(end / start, factor, 1e-4f));
            assert(clusterSlice(start * 1.001f, grid.sliceScale, grid.sliceBias, grid.slices) == slice);
            assert(clusterSlice(end * 0.999f, grid.sliceScale, grid.sliceBias, grid.slices) == slice);
        }

        assert(clusterSlice(0.05f, grid.sliceScale, grid.sliceBias, grid.slices) == 0);
        assert(clusterSlice(0.0f, grid.sliceScale, grid.sliceBias, grid.slices) == 0);
        assert(clusterSlice(150.0f, grid.sliceScale, grid.sliceBias, grid.slices) == grid.slices - 1);
    }

    // Numbered along a row, then down the screen, then into the depth; a pixel's tile is its pixel position over 64.
    void testClusterNumbering() {
        assert(clusterIndex(0, 0, 0, 40, 23) == 0);
        assert(clusterIndex(1, 0, 0, 40, 23) == 1);
        assert(clusterIndex(0, 1, 0, 40, 23) == 40);
        assert(clusterIndex(0, 0, 1, 40, 23) == 40 * 23);
        assert(clusterIndex(39, 22, 23, 40, 23) == 40 * 23 * 24 - 1);

        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 2560, 1440);
        const float depth = 3.0f;
        const int slice = clusterSlice(depth, grid.sliceScale, grid.sliceBias, grid.slices);

        assert(clusterOfPixel(grid, 0.5f, 0.5f, depth) == clusterIndex(0, 0, slice, 40, 23));
        assert(clusterOfPixel(grid, 63.5f, 0.5f, depth) == clusterIndex(0, 0, slice, 40, 23));
        assert(clusterOfPixel(grid, 64.5f, 0.5f, depth) == clusterIndex(1, 0, slice, 40, 23));
        assert(clusterOfPixel(grid, 2559.5f, 1439.5f, depth) == clusterIndex(39, 22, slice, 40, 23));
    }

    /*
    * The pixels the clusters assume are the ones the camera's projection gives, with row 0 at the top of the screen
    * (the renderer's viewport flips y): a point projected by the camera lands in the cluster whose box holds it.
    */
    void testClustersMatchTheProjection() {
        const Camera camera = tiltedCamera(16.0f / 9.0f);
        const ClusterGrid grid = makeClusterGrid(camera, 1280, 720);
        const Mat4 viewProjection = camera.getProjectionMatrix() * camera.getViewMatrix();
        TestRandom random;

        for (int i = 0; i < 2000; ++i) {
            const Vec3 relative = pointAtPixel(grid, random.between(0.0f, 1280.0f), random.between(0.0f, 720.0f), randomDepth(random, grid));
            const Vec3 world = camera.getPosition() + relative;
            const Vec4 clip = viewProjection * Vec4{world.x, world.y, world.z, 1.0f};
            const float pixelX = (clip.x / clip.w + 1.0f) * 0.5f * 1280.0f;
            const float pixelY = (1.0f - clip.y / clip.w) * 0.5f * 720.0f;

            const Vec3 view = toClusterView(grid, relative);
            assert(nearlyEqual(view.z, clip.w, 1e-4f * clip.w));

            const int tileX = static_cast<int>(pixelX) / clusterTilePixels;
            const int tileY = static_cast<int>(pixelY) / clusterTilePixels;
            const int slice = clusterSlice(view.z, grid.sliceScale, grid.sliceBias, grid.slices);
            assert(clusterOfPixel(grid, pixelX, pixelY, view.z) == clusterIndex(tileX, tileY, slice, grid.tilesX, grid.tilesY));

            const Aabb box = clusterBounds(grid, tileX, tileY, slice);
            assert(view.x >= box.minimum.x && view.x <= box.maximum.x);
            assert(view.y >= box.minimum.y && view.y <= box.maximum.y);
            assert(view.z >= box.minimum.z && view.z <= box.maximum.z);
        }
    }

    /*
    * A cluster's box holds its piece of the frustum and no more than the margin beyond: the far corner of the first
    * tile at the slice's far depth is a corner of the box, and the last tile stops at the screen's edge.
    */
    void testClusterBounds() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(1.0f), 100, 100);
        const int slice = 10;
        const float far = sliceStartDepth(slice + 1, grid.sliceScale, grid.sliceBias);
        const float margin = clusterBoundsMargin * far;

        const Aabb first = clusterBounds(grid, 0, 0, slice);
        assert(nearlyEqual(first.minimum.x, -far * grid.tanHalfWidth - margin, 1e-5f));
        assert(nearlyEqual(first.maximum.y, far * grid.tanHalfHeight + margin, 1e-5f));
        assert(nearlyEqual(first.maximum.z, far + margin, 1e-5f));
        assert(nearlyEqual(first.minimum.z, sliceStartDepth(slice, grid.sliceScale, grid.sliceBias) - margin, 1e-5f));

        // The second tile spans pixels 64 to 100, the right edge of the screen.
        const Aabb last = clusterBounds(grid, 1, 1, slice);
        assert(nearlyEqual(last.maximum.x, far * grid.tanHalfWidth + margin, 1e-5f));
        assert(nearlyEqual(last.minimum.y, -far * grid.tanHalfHeight - margin, 1e-5f));

        // The first slice starts at the camera.
        assert(nearlyEqual(clusterBounds(grid, 0, 0, 0).minimum.z, -clusterBoundsMargin * sliceStartDepth(1, grid.sliceScale, grid.sliceBias), 1e-7f));
    }

    void testSphereTouchesBox() {
        const Aabb box{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}};

        assert(sphereTouchesBox(Vec3{0.5f, 0.5f, 0.5f}, 0.1f, box));
        assert(sphereTouchesBox(Vec3{1.5f, 0.5f, 0.5f}, 0.6f, box));
        assert(!sphereTouchesBox(Vec3{1.5f, 0.5f, 0.5f}, 0.4f, box));

        // Past a corner the distance counts on every axis: sqrt(3) * 0.5 = 0.866 from (1.5, 1.5, 1.5).
        assert(!sphereTouchesBox(Vec3{1.5f, 1.5f, 1.5f}, 0.85f, box));
        assert(sphereTouchesBox(Vec3{1.5f, 1.5f, 1.5f}, 0.87f, box));
    }

    /*
    * The cone test misses a sphere outside the cone's angle, behind its apex or past its range, and touches one on the
    * axis or grazing the edge. A cone of 90 degrees or wider is tested by its range alone.
    */
    void testConeTouchesSphere() {
        const Vec3 apex{0.0f, 0.0f, 0.0f};
        const Vec3 axis{0.0f, 0.0f, 1.0f};
        const float cos30 = std::cos(radiansFromDegrees(30.0f));

        assert(coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{0.0f, 0.0f, 5.0f}, 0.5f));
        assert(!coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{0.0f, 0.0f, -5.0f}, 0.5f));
        assert(!coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{0.0f, 0.0f, 11.0f}, 0.5f));
        assert(coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{0.0f, 0.0f, 10.4f}, 0.5f));

        // At depth 5 the cone's edge is 5 tan 30 = 2.887 from the axis; the distance from the edge to a point is
        // measured at right angles to it, so a sphere centred at x = 4 is (4 - 2.887) cos 30 = 0.964 from the edge.
        assert(!coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{4.0f, 0.0f, 5.0f}, 0.9f));
        assert(coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{4.0f, 0.0f, 5.0f}, 1.0f));

        // A sphere around the apex always touches.
        assert(coneTouchesSphere(apex, axis, cos30, 10.0f, Vec3{0.0f, 0.3f, -0.2f}, 0.5f));

        // A half-angle past 90 degrees (wider than a hemisphere) is tested by its range alone.
        const float cos100 = std::cos(radiansFromDegrees(100.0f));
        assert(coneTouchesSphere(apex, axis, cos100, 10.0f, Vec3{0.0f, 0.0f, -5.0f}, 0.5f));
    }

    /*
    * The property everything rests on: every light that reaches a point is listed in that point's cluster, so a pixel
    * looping over its cluster's lights adds every light it added before (the rest gave nothing). Random lights around
    * random points all over the view, point lights by range, spotlights by range and cone.
    */
    void testEveryReachingLightIsListed() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 1280, 720);
        TestRandom random;

        for (int round = 0; round < 40; ++round) {
            GpuLights lights;
            std::vector<Vec3> points;

            for (int i = 0; i < 25; ++i) {
                const float depth = randomDepth(random, grid);
                const Vec3 point = pointAtPixel(grid, random.between(0.0f, 1280.0f), random.between(0.0f, 720.0f), depth);
                points.push_back(point);

                // A light reaching the point, from barely to well within its range.
                const float range = random.between(0.2f, 0.5f) * (1.0f + depth);
                const Vec3 position = point + random.direction() * (range * random.between(0.0f, 0.999f));
                addPoint(lights, pointLight(position, range));

                // A spotlight aimed near the point, with the point inside its cone.
                const Vec3 spotPosition = point + random.direction() * (range * random.between(0.05f, 0.999f));
                const Vec3 towardsPoint = (point - spotPosition).normalized();
                const float cosOuter = random.between(0.5f, 0.98f);
                const Vec3 aim = (towardsPoint + random.direction() * (0.5f * std::sqrt(1.0f - cosOuter * cosOuter))).normalized();

                if (aim.dot(towardsPoint) > cosOuter) {
                    addSpot(lights, spotLight(spotPosition, range, aim, cosOuter));
                } else {
                    addSpot(lights, spotLight(spotPosition, range, towardsPoint, cosOuter));
                }
            }

            const ClusterMasks masks = buildClusterMasks(grid, lights);

            for (std::size_t i = 0; i < points.size(); ++i) {
                const Vec3 view = toClusterView(grid, points[i]);
                const float pixelX = (view.x / (view.z * grid.tanHalfWidth) + 1.0f) * 0.5f * 1280.0f;
                const float pixelY = (1.0f - view.y / (view.z * grid.tanHalfHeight)) * 0.5f * 720.0f;
                const int cluster = clusterOfPixel(grid, pixelX, pixelY, view.z);

                for (std::size_t light = 0; light < lights.points.size(); ++light) {
                    const GpuPointLight& record = lights.points[light];
                    const Vec3 position{record.positionRange[0], record.positionRange[1], record.positionRange[2]};

                    if ((points[i] - position).length() < record.positionRange[3]) {
                        assert(masks.hasPointLight(cluster, static_cast<int>(light)));
                    }
                }

                for (std::size_t light = 0; light < lights.spots.size(); ++light) {
                    const GpuSpotLight& record = lights.spots[light];
                    const Vec3 position{record.positionRange[0], record.positionRange[1], record.positionRange[2]};
                    const Vec3 direction{record.directionCosOuter[0], record.directionCosOuter[1], record.directionCosOuter[2]};
                    const Vec3 offset = points[i] - position;
                    const float distance = offset.length();

                    if (distance < record.positionRange[3] && (offset / distance).dot(direction) > record.directionCosOuter[3]) {
                        assert(masks.hasSpotLight(cluster, static_cast<int>(light)));
                    }
                }
            }
        }
    }

    /*
    * And the point of it all: a light is listed only near where it reaches. A lamp of range 5 a few metres in front of
    * the camera is in a small share of the grid's clusters, and in none whose box its sphere misses; a narrow spotlight
    * is in fewer clusters than a point light of the same range at the same place.
    */
    void testLightsAreListedOnlyNearby() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 1280, 720);
        const Vec3 position = grid.forward * 8.0f + grid.right * 2.0f;

        GpuLights lights;
        addPoint(lights, pointLight(position, 5.0f));
        addSpot(lights, spotLight(position, 5.0f, grid.right, std::cos(radiansFromDegrees(15.0f))));

        const ClusterMasks masks = buildClusterMasks(grid, lights);
        const Vec3 viewPosition = toClusterView(grid, position);
        int pointClusters = 0;
        int spotClusters = 0;

        for (int slice = 0; slice < grid.slices; ++slice) {
            for (int tileY = 0; tileY < grid.tilesY; ++tileY) {
                for (int tileX = 0; tileX < grid.tilesX; ++tileX) {
                    const int cluster = clusterIndex(tileX, tileY, slice, grid.tilesX, grid.tilesY);
                    const bool touches = sphereTouchesBox(viewPosition, 5.0f, clusterBounds(grid, tileX, tileY, slice));
                    assert(masks.hasPointLight(cluster, 0) == touches);
                    pointClusters += touches ? 1 : 0;
                    spotClusters += masks.hasSpotLight(cluster, 0) ? 1 : 0;

                    // A spotlight is listed only where its range sphere reaches too.
                    assert(!masks.hasSpotLight(cluster, 0) || touches);
                }
            }
        }

        assert(pointClusters > 0);
        assert(pointClusters < clusterCount(grid) / 5);
        assert(spotClusters > 0);
        assert(spotClusters < pointClusters / 2);
    }

    // Only the lights the frame uses are listed: records past the counts leave their bits clear.
    void testOnlyCountedLightsAreListed() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 640, 360);
        const Vec3 position = grid.forward * 3.0f;

        GpuLights lights;
        addPoint(lights, pointLight(position, 50.0f));
        addPoint(lights, pointLight(position, 50.0f));
        addSpot(lights, spotLight(position, 50.0f, grid.forward, 0.5f));
        lights.counts[1] = 1;
        lights.counts[2] = 0;

        const ClusterMasks masks = buildClusterMasks(grid, lights);
        assert(masks.words.size() == static_cast<std::size_t>(clusterCount(grid) * clusterWords));

        const int cluster = clusterOfPixel(grid, 320.0f, 180.0f, 3.0f);
        assert(masks.hasPointLight(cluster, 0));
        assert(!masks.hasPointLight(cluster, 1));
        assert(!masks.hasSpotLight(cluster, 0));
    }

    // One word per 32 lights for every cluster: 160 bytes each, about 3.5 MB at 2560 x 1440.
    void testMaskBufferSize() {
        const ClusterGrid grid = makeClusterGrid(tiltedCamera(16.0f / 9.0f), 2560, 1440);
        assert(clusterMaskBytes(grid) == static_cast<std::size_t>(40 * 23 * 24 * 40 * 4));
        assert(clusterMaskBytes(2560, 1440) == clusterMaskBytes(grid));
    }

    // Shading from the clusters is on by default and switchable while the game runs, to compare the two in the GPU timings.
    void testClusteredShadingIsOnByDefaultAndSwitchesLive() {
        const RenderSettings defaults;
        assert(defaults.clusteredShading);

        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings = applySettings({{"passes.clusteredShading", "false"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(!settings.clusteredShading);
    }

    /*
    * The heatmap's colours: black for a cluster with no lights, then blue, green, yellow and red as the count grows to
    * clusterHeatmapLights, and red beyond. Each stop is a pure colour, and the ramp moves through them in order.
    */
    void testHeatmapColours() {
        const auto same = [](const Vec3& actual, const Vec3& expected) {
            return nearlyEqual(actual.x, expected.x, 1e-6f) && nearlyEqual(actual.y, expected.y, 1e-6f) && nearlyEqual(actual.z, expected.z, 1e-6f);
        };

        const float third = static_cast<float>(clusterHeatmapLights) / 3.0f;
        assert(same(clusterHeatColour(0), Vec3{0.0f, 0.0f, 0.0f}));
        assert(same(clusterHeatColour(clusterHeatmapLights), Vec3{1.0f, 0.0f, 0.0f}));
        assert(same(clusterHeatColour(clusterHeatmapLights * 4), Vec3{1.0f, 0.0f, 0.0f}));

        // One light is nearly pure blue, a third of the scale green, two thirds yellow.
        assert(clusterHeatColour(1).z > 0.8f && clusterHeatColour(1).x == 0.0f);
        assert(same(clusterHeatColour(static_cast<int>(third)), clusterHeatRamp(static_cast<float>(static_cast<int>(third)) / static_cast<float>(clusterHeatmapLights))));
        assert(same(clusterHeatRamp(1.0f / 3.0f), Vec3{0.0f, 1.0f, 0.0f}));
        assert(same(clusterHeatRamp(2.0f / 3.0f), Vec3{1.0f, 1.0f, 0.0f}));
        assert(same(clusterHeatRamp(0.0f), Vec3{0.0f, 0.0f, 1.0f}));

        // Red rises before green falls: from green to yellow red grows, from yellow to red green shrinks.
        assert(clusterHeatRamp(0.5f).x > 0.0f && clusterHeatRamp(0.5f).y == 1.0f);
        assert(clusterHeatRamp(0.8f).x == 1.0f && clusterHeatRamp(0.8f).y < 1.0f);
    }

    // The heatmap is a debug view: off by default, switchable live.
    void testHeatmapIsOffByDefault() {
        assert(!RenderSettings{}.clusterHeatmap);

        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings = applySettings({{"passes.clusterHeatmap", "true"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(settings.clusterHeatmap);
    }
}

void testClusters() {
    testGridSize();
    testGridFollowsTheCamera();
    testSlicesAreExponential();
    testClusterNumbering();
    testClustersMatchTheProjection();
    testClusterBounds();
    testSphereTouchesBox();
    testConeTouchesSphere();
    testEveryReachingLightIsListed();
    testLightsAreListedOnlyNearby();
    testOnlyCountedLightsAreListed();
    testMaskBufferSize();
    testClusteredShadingIsOnByDefaultAndSwitchesLive();
    testHeatmapColours();
    testHeatmapIsOffByDefault();
}
