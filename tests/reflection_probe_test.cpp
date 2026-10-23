#include "math/mat4.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/soft_shadow.h"
#include "render/gpu/taa.h"

#include "test_levels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <functional>
#include <numbers>

namespace {
    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    // Directions spread over the sphere (a Fibonacci spiral), none along an axis or on an edge between faces.
    std::array<Vec3, 200> spreadDirections() {
        std::array<Vec3, 200> directions{};
        const float golden = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));

        for (std::size_t i = 0; i < directions.size(); ++i) {
            const float y = 1.0f - (static_cast<float>(i) + 0.5f) * 2.0f / static_cast<float>(directions.size());
            const float radius = std::sqrt(1.0f - y * y);
            const float angle = golden * static_cast<float>(i) + 0.1f;
            directions[i] = Vec3{radius * std::cos(angle), y, radius * std::sin(angle)};
        }

        return directions;
    }

    /*
    * Vulkan's cube addressing, from the specification's table: +X is face 0 with s along -z and t along -y; the face
    * centres are their axes. A direction and its texel turn into each other both ways.
    */
    void testCubeAddressing() {
        const CubeTexel plusX = cubeTexelOf(Vec3{1.0f, 0.5f, 0.0f});
        assert(plusX.face == 0 && near(plusX.st.x, 0.5f, 1e-6f) && near(plusX.st.y, 0.25f, 1e-6f));

        const CubeTexel minusZ = cubeTexelOf(Vec3{0.25f, 0.0f, -1.0f});
        assert(minusZ.face == 5 && near(minusZ.st.x, 0.375f, 1e-6f) && near(minusZ.st.y, 0.5f, 1e-6f));

        const CubeTexel plusY = cubeTexelOf(Vec3{0.0f, 2.0f, 1.0f});
        assert(plusY.face == 2 && near(plusY.st.x, 0.5f, 1e-6f) && near(plusY.st.y, 0.75f, 1e-6f));

        for (int face = 0; face < cubeFaceCount; ++face) {
            const CubeFaceAxes axes = cubeFaceAxes(face);
            assert(cubeTexelOf(axes.forward).face == face);
            assert(near(axes.right.dot(axes.forward), 0.0f, 1e-6f) && near(axes.up.dot(axes.forward), 0.0f, 1e-6f));
        }

        for (const Vec3& direction : spreadDirections()) {
            const CubeTexel texel = cubeTexelOf(direction);
            assert(texel.st.x >= 0.0f && texel.st.x <= 1.0f && texel.st.y >= 0.0f && texel.st.y <= 1.0f);
            assert(nearVec(cubeTexelDirection(texel.face, texel.st).normalized(), direction, 1e-5f));
        }
    }

    /*
    * Each face's view-projection puts a direction at the pixel the cube stores it at, the near plane at depth 1 and the
    * far one at 0; a triangle facing the probe turns the other way round on screen (the faces are mirror images).
    */
    void testFaceViews() {
        constexpr float nearPlane = 0.05f;
        constexpr float farPlane = 200.0f;
        const Vec2 size{128.0f, 128.0f};

        for (const Vec3& direction : spreadDirections()) {
            const CubeTexel texel = cubeTexelOf(direction);
            const Mat4 view = cubeFaceViewProjection(texel.face, nearPlane, farPlane);
            const Vec3 point = direction * 7.0f;
            const Vec4 clip = view * Vec4{point.x, point.y, point.z, 1.0f};
            const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
            assert(near(pixel.x, texel.st.x * size.x, 1e-3f) && near(pixel.y, texel.st.y * size.y, 1e-3f));

            const float axisDistance = cubeFaceAxes(texel.face).forward.dot(point);
            assert(near(clip.w, axisDistance, 1e-4f));
            assert(near(linearShadowDepth(clip.z / clip.w, nearPlane, farPlane), axisDistance, 1e-3f));
            assert(near(probeRadialDistance(axisDistance, direction), 7.0f, 1e-4f));
        }

        for (int face = 0; face < cubeFaceCount; ++face) {
            const CubeFaceAxes axes = cubeFaceAxes(face);
            const Mat4 view = cubeFaceViewProjection(face, nearPlane, farPlane);
            const auto depthOf = [&](float distance) {
                const Vec3 point = axes.forward * distance;
                const Vec4 clip = view * Vec4{point.x, point.y, point.z, 1.0f};
                return clip.z / clip.w;
            };
            assert(near(depthOf(nearPlane), 1.0f, 1e-5f) && near(depthOf(farPlane), 0.0f, 1e-5f));

            // A triangle in front of the probe, turning from right to up. Its normal by the right-hand rule (right x up)
            // points away from the probe, so seen from the probe it turns clockwise in the world...
            const std::array<Vec3, 3> corners{axes.forward * 2.0f, axes.forward * 2.0f + axes.right * 0.5f, axes.forward * 2.0f + axes.up * 0.5f};
            assert(axes.right.cross(axes.up).dot(axes.forward) > 0.0f);
            std::array<Vec2, 3> pixels{};

            for (std::size_t i = 0; i < corners.size(); ++i) {
                const Vec4 clip = view * Vec4{corners[i].x, corners[i].y, corners[i].z, 1.0f};
                pixels[i] = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
            }

            // ... but counter-clockwise on screen (right, then up the picture: in pixels, y down, a negative signed area).
            const float area = (pixels[1].x - pixels[0].x) * (pixels[2].y - pixels[0].y) - (pixels[2].x - pixels[0].x) * (pixels[1].y - pixels[0].y);
            assert(area < 0.0f);
        }
    }

    // The sky's camera rays for a face go through the same directions its picture holds.
    void testFaceRays() {
        for (int face = 0; face < cubeFaceCount; ++face) {
            const GpuRayDebugView rays = packCubeFaceRays(face, Vec3{1.0f, 2.0f, 3.0f}, 64);
            assert(rays.screen[0] == 64.0f && rays.screen[1] == 64.0f && rays.originAndView[1] == 2.0f);
            const Vec3 right{rays.right[0], rays.right[1], rays.right[2]};
            const Vec3 up{rays.up[0], rays.up[1], rays.up[2]};
            const Vec3 forward{rays.forward[0], rays.forward[1], rays.forward[2]};

            for (const Vec2 st : {Vec2{0.5f, 0.5f}, Vec2{0.1f, 0.8f}, Vec2{0.9f, 0.2f}}) {
                const Vec3 ray = cameraRayDirection(right, up, forward, 2.0f * st.x - 1.0f, 1.0f - 2.0f * st.y);
                assert(nearVec(ray.normalized(), cubeTexelDirection(face, st).normalized(), 1e-5f));
            }
        }
    }

    // How far behind what the probe saw the ray's point is (negative: in front), and the confidence in a crossing.
    void testParallaxMarch() {
        assert(near(parallaxMismatch(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f), std::sqrt(2.0f) - 1.0f, 1e-6f));
        assert(near(parallaxMismatch(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 3.0f), std::sqrt(2.0f) - 3.0f, 1e-6f));

        assert(nearVec(parallaxDirection(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f), Vec3{1.0f, 1.0f, 0.0f}.normalized(), 1e-6f));
        assert(nearVec(parallaxDirection(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, -1.0f), Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));

        assert(parallaxConfidence(10.0f, 10.0f) == 1.0f);
        assert(parallaxConfidence(10.0f * (1.0f + PROBE_MATCH_SHARE * 0.5f), 10.0f) == 1.0f);
        assert(parallaxConfidence(10.0f * (1.0f + PROBE_MISMATCH_SHARE * 1.5f), 10.0f) == 0.0f);
        assert(parallaxConfidence(10.0f * (1.0f - PROBE_MISMATCH_SHARE * 1.5f), 10.0f) == 0.0f);
    }

    // The distance the probe at the origin sees to a wall at z = -5 with a doorway (|x| < 0.5, below y = 2) through to a
    // back wall at z = -10, and sky (the far plane) behind it.
    float doorwayWall(const Vec3& direction) {
        constexpr float far = 300.0f;

        if (direction.z > -1e-3f) {
            return far;
        }

        const Vec3 onWall = direction * (5.0f / -direction.z);
        const bool throughDoorway = std::fabs(onWall.x) < 0.5f && onWall.y < 2.0f;
        return std::min(throughDoorway ? 10.0f / -direction.z : 5.0f / -direction.z, far);
    }

    // The distance the probe at the origin sees from a doorway in a wall it stands in: the wall 0.3 m thick (z from -0.15
    // to 0.15) except the doorway (|x| < 0.5), rooms 5 m deep either side, a floor and ceiling 1.5 m below and above.
    float standingInDoorway(const Vec3& direction) {
        float nearest = 300.0f;
        const auto keep = [&](float distance) {
            if (distance > 0.0f) {
                nearest = std::min(nearest, distance);
            }
        };

        if (std::fabs(direction.z) > 1e-6f) {
            keep(5.0f / std::fabs(direction.z));

            // The wall's faces at z = +-0.15, where they are wall and not doorway.
            for (const float face : {-0.15f, 0.15f}) {
                const float distance = face / direction.z;
                if (distance > 0.0f && std::fabs(direction.x * distance) >= 0.5f) {
                    keep(distance);
                }
            }
        }

        if (std::fabs(direction.y) > 1e-6f) {
            keep(1.5f / std::fabs(direction.y));
        }

        // The jambs at x = +-0.5, where they are inside the wall's thickness.
        if (std::fabs(direction.x) > 1e-6f) {
            const float distance = 0.5f / std::fabs(direction.x);
            if (std::fabs(direction.z * distance) <= 0.15f) {
                keep(distance);
            }
        }

        return nearest;
    }

    // A probe's distances as the GPU keeps them, from a scene's distance per direction, read through the search.
    struct TestProbe {
        ProbeDistanceCube cube;

        explicit TestProbe(const std::function<float(const Vec3&)>& scene) : cube(buildProbeDistanceCube(256, scene)) {}

        ParallaxLookup search(const Vec3& position, const Vec3& direction) const {
            return searchProbe(position, direction, 300.0f, cube.size, static_cast<std::uint32_t>(cube.levels.size()),
                               [&](const Vec3& towards, std::uint32_t level) { return cube.at(towards, level); },
                               [&](const Vec3& towards) { return cube.filtered(towards); }, [&](const Vec3& towards) { return cube.spread(towards); });
        }
    };

    // A cube's levels: the distance at each texel's centre, then the nearest of the four below at each level up.
    void testProbeDistanceCube() {
        const ProbeDistanceCube cube = buildProbeDistanceCube(16, [](const Vec3& direction) { return 2.0f + direction.x; });
        assert(cube.size == 16 && cube.levels.size() == 5);
        assert(cube.levels[4][0].size() == 1 && cube.levels[0][0].size() == 256);

        for (int face = 0; face < cubeFaceCount; ++face) {
            for (std::uint32_t level = 1; level < cube.levels.size(); ++level) {
                const std::uint32_t side = 16u >> level;

                for (std::uint32_t y = 0; y < side; ++y) {
                    for (std::uint32_t x = 0; x < side; ++x) {
                        const auto& below = cube.levels[level - 1][static_cast<std::size_t>(face)];
                        const std::uint32_t belowSide = side * 2;
                        const float nearest = std::min({below[2 * y * belowSide + 2 * x], below[2 * y * belowSide + 2 * x + 1],
                                                        below[(2 * y + 1) * belowSide + 2 * x], below[(2 * y + 1) * belowSide + 2 * x + 1]});
                        assert(cube.levels[level][static_cast<std::size_t>(face)][y * side + x] == nearest);
                    }
                }
            }
        }

        // Filtered, a direction between texel centres blends the four round it: at a centre, that texel alone.
        const ProbeDistanceCube ramp = buildProbeDistanceCube(16, [](const Vec3& direction) { return 2.0f + direction.x; });
        const Vec3 centre = cubeTexelDirection(4, Vec2{(5.0f + 0.5f) / 16.0f, (7.0f + 0.5f) / 16.0f}).normalized();
        assert(near(ramp.filtered(centre), ramp.at(centre, 0), 1e-5f));
        const Vec3 between = cubeTexelDirection(4, Vec2{6.0f / 16.0f, (7.0f + 0.5f) / 16.0f}).normalized();
        const Vec3 corner = cubeTexelDirection(4, Vec2{6.0f / 16.0f, 7.0f / 16.0f}).normalized();
        const float lowest = std::min({ramp.at(cubeTexelDirection(4, Vec2{5.5f / 16.0f, 6.5f / 16.0f}).normalized(), 0),
                                       ramp.at(cubeTexelDirection(4, Vec2{5.5f / 16.0f, 7.5f / 16.0f}).normalized(), 0)});
        const float highest = std::max({ramp.at(cubeTexelDirection(4, Vec2{6.5f / 16.0f, 6.5f / 16.0f}).normalized(), 0),
                                        ramp.at(cubeTexelDirection(4, Vec2{6.5f / 16.0f, 7.5f / 16.0f}).normalized(), 0), lowest});
        assert(near(ramp.spread(corner), (highest - lowest) / highest, 1e-5f));
        const Vec3 left = cubeTexelDirection(4, Vec2{(5.0f + 0.5f) / 16.0f, (7.0f + 0.5f) / 16.0f}).normalized();
        const Vec3 right = cubeTexelDirection(4, Vec2{(6.0f + 0.5f) / 16.0f, (7.0f + 0.5f) / 16.0f}).normalized();
        assert(near(ramp.filtered(between), 0.5f * (ramp.at(left, 0) + ramp.at(right, 0)), 1e-5f));

        // Looking up a direction reads its texel at that level; the top level holds each face's nearest.
        assert(near(cube.at(Vec3{1.0f, 0.0f, 0.0f}, 0), 2.0f + 1.0f, 0.01f));
        assert(cube.at(Vec3{1.0f, 0.0f, 0.0f}, 4) <= cube.at(Vec3{1.0f, 0.0f, 0.0f}, 0));

        assert(near(probeTexelAngle(256), 0.5f * std::numbers::pi_v<float> / 256.0f, 1e-9f));

        /*
        * Cells and where a ray leaves one: from (0.2, 0, -1) along +x, the -Z face (whose columns run along -x) sees the
        * ray at sc = -0.2 - t, so with 2 cells a side it is in the left one (sc from -1 to 0) and leaves it at the
        * face's edge, t = 0.8; with 4 cells (sc from -0.5 to 0) at t = 0.3; a ray along the line of sight never leaves.
        */
        const Vec3 start{0.2f, 0.0f, -1.0f};
        const Vec3 along{1.0f, 0.0f, 0.0f};
        const CubeCell wide = cubeCellOf(start.normalized(), 2);
        assert(wide.face == 5 && wide.x == 0);
        assert(near(cubeCellExit(start, along, 0.0f, wide, 2, 100.0f), 0.8f, 1e-3f) && cubeCellExit(start, along, 0.0f, wide, 2, 100.0f) > 0.8f);
        const CubeCell narrow = cubeCellOf(start.normalized(), 4);
        assert(narrow.x == 1 && near(cubeCellExit(start, along, 0.0f, narrow, 4, 100.0f), 0.3f, 1e-3f));
        assert(cubeCellExit(start, start.normalized(), 0.0f, narrow, 4, 100.0f) == 100.0f);
        assert(cubeCellExit(start, along, 0.0f, wide, 2, 0.5f) == 0.5f);
    }

    /*
    * The search against surroundings the probe saw: a sphere round it, a wall and sky, a doorway the ray passes beside
    * (the first surface it meets is the wall, though the probe sees the room behind the doorway in the ray's own
    * direction), a doorway it passes through, a world the probe could not have seen, an edge the probe sees past, and
    * a wall the probe sees edge-on, standing in its doorway: the march must not step over it into the next room.
    */
    void testSearchProbe() {
        /*
        * A floor 1.5 m below the probe, seen at a slant: its distance changes a lot from texel to texel, yet it is one
        * continuous surface. Mirror rays from a wall 3 m away come down onto it at every steepness, the shallow ones
        * running over it a long way; each is found and trusted within the walk's steps.
        */
        const TestProbe floorOnly{[](const Vec3& direction) { return direction.y < 0.0f ? std::min(1.5f / -direction.y, 300.0f) : 300.0f; }};

        for (int ray = 0; ray < 30; ++ray) {
            const float drop = 0.05f + 0.03f * static_cast<float>(ray);
            const Vec3 down = Vec3{0.3f * std::sin(static_cast<float>(ray)), -drop, 1.0f}.normalized();
            const Vec3 fromWall{0.0f, 0.2f, -3.0f};
            const ParallaxLookup onFloor = floorOnly.search(fromWall, down);
            const Vec3 hit = fromWall + down * (1.7f / -down.y);
            assert(onFloor.confidence > 0.9f && nearVec(onFloor.direction, hit.normalized(), 0.01f));
        }

        const TestProbe sphere{[](const Vec3&) { return 10.0f; }};
        const ParallaxLookup onSphere = sphere.search(Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
        assert(nearVec(onSphere.direction, Vec3{2.0f, 0.0f, std::sqrt(96.0f)}.normalized(), 0.01f) && onSphere.confidence > 0.9f);

        const TestProbe doorway{doorwayWall};
        const Vec3 shiny{3.0f, -1.0f, 0.0f};
        const Vec3 towardsWall = Vec3{0.3f, 0.2f, -1.0f}.normalized();
        const ParallaxLookup onWall = doorway.search(shiny, towardsWall);
        const Vec3 wallHit = shiny + towardsWall * (5.0f / -towardsWall.z);
        assert(nearVec(onWall.direction, wallHit.normalized(), 0.01f) && onWall.confidence > 0.9f);

        const ParallaxLookup onSky = doorway.search(shiny, Vec3{0.0f, 0.6f, 0.8f});
        assert(nearVec(onSky.direction, Vec3{0.0f, 0.6f, 0.8f}, 0.02f) && onSky.confidence > 0.9f);

        // Beside the doorway: the probe sees through it in the ray's own direction (x/z = 0.05), but the ray, starting
        // 3 m to the side, meets the wall at x = 3.25.
        const ParallaxLookup besideDoorway = doorway.search(Vec3{3.0f, 0.0f, 0.0f}, Vec3{0.05f, 0.0f, -1.0f}.normalized());
        assert(nearVec(besideDoorway.direction, Vec3{3.25f, 0.0f, -5.0f}.normalized(), 0.01f) && besideDoorway.confidence > 0.9f);

        // Through the doorway to the back wall.
        const ParallaxLookup throughDoorway = doorway.search(Vec3{0.2f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});
        assert(nearVec(throughDoorway.direction, Vec3{0.2f, 0.0f, -10.0f}.normalized(), 0.01f) && throughDoorway.confidence > 0.9f);

        // Everything the probe saw is nearer than any point of the ray: nothing to find.
        const TestProbe close{[](const Vec3&) { return 2.0f; }};
        assert(close.search(Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}).confidence == 0.0f);

        /*
        * Behind an edge the probe cannot see past: a ray through the doorway at a slant, on into the back room behind
        * the wall, where the probe sees only the wall's near face. It goes behind what the probe saw where the probe's
        * line of sight leaves the doorway: the distances on either side jump from the back wall to the wall, so the
        * crossing is no surface and must not be trusted.
        */
        assert(doorway.search(Vec3{0.0f, 0.0f, -4.9f}, Vec3{1.0f, 0.0f, -1.0f}.normalized()).confidence < 0.1f);

        /*
        * Standing in a doorway, a mirror 4 m in front reflects a ray onto the wall's face just beside the jamb. The probe
        * sees that face edge-on, hidden behind the jamb: it cannot tell what is there. A few tens of centimetres on, past
        * the wall, the ray is back in view in the room beyond, so a march stepping over the wall would find that room.
        */
        const TestProbe inDoorway{standingInDoorway};
        assert(inDoorway.search(Vec3{-0.8f, 0.0f, 4.0f}, Vec3{0.05f, 0.0f, -1.0f}.normalized()).confidence < 0.1f);

        /*
        * A post 80 m away seen from a point 30 cm from the probe: the ray and the probe look at it along the same line
        * (within a texel), so the probe sees what the ray sees even where the distances jump from the post to the sky
        * at its silhouette. Every ray sweeping across its edge is trusted.
        */
        const TestProbe farPost{[](const Vec3& direction) {
            return direction.z < 0.0f && std::fabs(std::atan2(direction.x, -direction.z)) < 0.02f ? 80.0f / -direction.z : 300.0f;
        }};

        for (int ray = 0; ray <= 20; ++ray) {
            const float angle = 0.015f + 0.0005f * static_cast<float>(ray);
            const Vec3 across{std::sin(angle), 0.0f, -std::cos(angle)};
            assert(farPost.search(Vec3{0.3f, 0.0f, 0.0f}, across).confidence > 0.9f);
        }

        /*
        * A ray from a slab on the ground, 4 m ahead of a probe at eye height, rising towards the sky over the ground
        * beyond: it never meets anything, and once the probe's line of sight to it is within a texel of its own, the
        * probe sees what it sees. Found along its own direction, and trusted.
        */
        const TestProbe ground{[](const Vec3& direction) { return direction.y < 0.0f ? std::min(1.5f / -direction.y, 300.0f) : 300.0f; }};

        for (const float rise : {0.05f, 0.2f, 0.4f}) {
            const Vec3 skimming = Vec3{0.0f, rise, -1.0f}.normalized();
            const ParallaxLookup horizon = ground.search(Vec3{0.0f, -1.46f, -4.0f}, skimming);
            assert(horizon.confidence > 0.9f && nearVec(horizon.direction, skimming, 0.01f));
        }

        /*
        * The start's lift off its surface: a couple of the probe's texels at the point's distance, more where the probe
        * sees the surface at a slant (its distances there change fast from texel to texel), up to a limit.
        */
        const float texelAt4 = probeTexelAngle(256) * 4.0f;
        const Vec3 up{0.0f, 1.0f, 0.0f};
        assert(near(probeStartLift(Vec3{0.0f, -4.0f, 0.0f}, up, 256), static_cast<float>(PROBE_START_LIFT_TEXELS) * texelAt4, 1e-6f));
        assert(probeStartLift(Vec3{0.0f, -1.0f, -3.9f}, up, 256) > probeStartLift(Vec3{0.0f, -4.0f, 0.0f}, up, 256));
        assert(near(probeStartLift(Vec3{0.0f, 0.0f, -4.0f}, up, 256),
                    static_cast<float>(PROBE_START_LIFT_TEXELS) * texelAt4 / static_cast<float>(PROBE_START_LIFT_SLANT), 1e-6f));

        /*
        * From a slab on the ground seen at a slant from the probe, rays rising at every angle towards the sky must not
        * catch on the ground's coarse texels near their start: lifted, every one is found along its own direction.
        */
        const Vec3 onSlab{0.0f, -1.46f, -4.0f};
        const Vec3 lifted = onSlab + up * probeStartLift(onSlab, up, 256);

        for (int ray = 0; ray < 40; ++ray) {
            const float rise = 0.05f + 0.02f * static_cast<float>(ray);
            const Vec3 skyward = Vec3{0.1f * std::sin(static_cast<float>(ray)), rise, -1.0f}.normalized();
            const ParallaxLookup sky = ground.search(lifted, skyward);
            assert(sky.confidence > 0.9f && nearVec(sky.direction, skyward, 0.01f));
        }

        // A surface or an edge: four texels spread a little are a surface, spread far an edge between two.
        assert(probeEdgeConfidence(0.0f) == 1.0f && probeEdgeConfidence(PROBE_EDGE_JUMP_START) == 1.0f);
        assert(probeEdgeConfidence(PROBE_EDGE_JUMP_END) == 0.0f && probeEdgeConfidence(0.5f) == 0.0f);

        // The view's agreement: full within a texel of the ray's own direction, none past two.
        const float texel = probeTexelAngle(256);
        assert(probeViewConfidence(0.0f, 256) == 1.0f && probeViewConfidence(texel, 256) == 1.0f);
        assert(probeViewConfidence(2.0f * texel, 256) == 0.0f);

        // ... but the floor in front of it, in plain view, it finds.
        const Vec3 towardsFloor = Vec3{0.0f, -1.0f, -1.0f}.normalized();
        const ParallaxLookup floor = inDoorway.search(Vec3{0.0f, 0.0f, 4.0f}, towardsFloor);
        assert(nearVec(floor.direction, Vec3{0.0f, -1.5f, 2.5f}.normalized(), 0.01f) && floor.confidence > 0.9f);
    }

    // Levels down to the smallest size, roughness spread evenly over them, and the source level a sample reads.
    void testLevels() {
        assert(probeLevels(256) == 7 && probeLevels(128) == 6 && probeLevels(64) == 5);
        assert(probeLevels(4) == 1 && probeLevels(2) == 1);

        assert(probeLevelRoughness(0, 7) == 0.0f && probeLevelRoughness(6, 7) == 1.0f && near(probeLevelRoughness(3, 7), 0.5f, 1e-6f));
        assert(probeLevelRoughness(0, 1) == 0.0f);
        assert(probeRoughnessLevel(0.0f, 7) == 0.0f && probeRoughnessLevel(1.0f, 7) == 6.0f && near(probeRoughnessLevel(0.25f, 7), 1.5f, 1e-6f));

        // A sample standing for exactly one top-level texel reads one level down; four times the solid angle, one more.
        const float texel = 4.0f * std::numbers::pi_v<float> / (6.0f * 128.0f * 128.0f);
        const float pdf = 1.0f / (64.0f * texel);
        assert(near(prefilterSourceLevel(pdf, 64, 128), 1.0f, 1e-4f));
        assert(near(prefilterSourceLevel(pdf / 4.0f, 64, 128), 2.0f, 1e-4f));
        assert(prefilterSourceLevel(pdf * 1e6f, 64, 128) == 0.0f);
    }

    // A uniform world stays uniform at every roughness; a sharp level reads only its own direction; a world lit from
    // above is brighter looking up than looking down.
    void testPrefilter() {
        const auto uniform = [](const Vec3&, float) { return Vec3{0.3f, 0.5f, 0.7f}; };
        const Vec3 direction = Vec3{0.2f, 0.9f, -0.4f}.normalized();

        for (const float roughness : {0.0f, 0.3f, 1.0f}) {
            assert(nearVec(prefilterTexel(direction, roughness, 64, 128, uniform), Vec3{0.3f, 0.5f, 0.7f}, 1e-5f));
        }

        const auto pointing = [](const Vec3& read, float) { return read; };
        assert(nearVec(prefilterTexel(direction, 0.0f, 64, 128, pointing), direction, 1e-5f));

        const auto sky = [](const Vec3& read, float) { return read.y > 0.0f ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{}; };
        const Vec3 lookingUp = prefilterTexel(Vec3{0.0f, 1.0f, 0.0f}, 0.6f, 64, 128, sky);
        const Vec3 lookingDown = prefilterTexel(Vec3{0.0f, -1.0f, 0.0f}, 0.6f, 64, 128, sky);
        assert(lookingUp.x > 0.95f && lookingDown.x < 0.05f);
    }

    // The probe's light in place of the stand-in, as far as it is trusted.
    void testBlendedStandIn() {
        const Vec3 standIn{0.2f, 0.2f, 0.2f};
        const Vec3 albedo{0.5f, 0.5f, 0.5f};
        const Vec3 light{2.0f, 1.0f, 0.0f};
        assert(nearVec(blendedStandIn(standIn, albedo, light, 0.0f), standIn, 0.0f));
        assert(nearVec(blendedStandIn(standIn, albedo, light, 1.0f), Vec3{1.0f, 0.5f, 0.0f}, 1e-6f));
        assert(nearVec(blendedStandIn(standIn, albedo, light, 0.5f), Vec3{0.6f, 0.35f, 0.1f}, 1e-6f));

        const GpuPrefilterPass pass = packPrefilterPass(32, 2, 6, 128, 8);
        assert(pass.target[0] == 32.0f && pass.target[1] == 2.0f && near(pass.target[2], 0.4f, 1e-6f));
        assert(pass.source[0] == 128.0f && pass.source[1] == 8.0f);
    }
}

void testReflectionProbe() {
    testCubeAddressing();
    testFaceViews();
    testFaceRays();
    testParallaxMarch();
    testProbeDistanceCube();
    testSearchProbe();
    testLevels();
    testPrefilter();
    testBlendedStandIn();
}
