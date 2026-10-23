#include "math/mat4.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/soft_shadow.h"
#include "render/gpu/taa.h"

#include "test_levels.h"

#include <array>
#include <cassert>
#include <cmath>
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

    /*
    * How far behind what the probe saw the ray's point is (negative: in front), and the march's steps: growing by the
    * same factor each step from the start to the end.
    */
    void testParallaxMarch() {
        assert(near(parallaxMismatch(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f), std::sqrt(2.0f) - 1.0f, 1e-6f));
        assert(near(parallaxMismatch(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 3.0f), std::sqrt(2.0f) - 3.0f, 1e-6f));

        const float ratio = probeMarchRatio(300.0f);
        float t = static_cast<float>(PROBE_MARCH_START);

        for (int step = 0; step < PROBE_MARCH_STEPS; ++step) {
            t *= ratio;
        }

        assert(near(t, 300.0f, 0.01f));

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

    /*
    * The search against surroundings the probe saw: a sphere round it, a wall and sky, a doorway the ray passes beside
    * (the first surface it meets is the wall, though the probe sees the room behind the doorway in the ray's own
    * direction), a doorway it passes through, and a world the probe could not have seen, which it must not trust.
    */
    void testSearchProbe() {
        constexpr float far = 300.0f;
        const Vec3 position{2.0f, 0.0f, 0.0f};
        const Vec3 up{0.0f, 0.0f, 1.0f};
        const ParallaxLookup sphere = searchProbe(position, up, far, [](const Vec3&) { return 10.0f; });
        assert(nearVec(sphere.direction, Vec3{2.0f, 0.0f, std::sqrt(96.0f)}.normalized(), 2e-3f) && sphere.confidence > 0.9f);

        const Vec3 shiny{3.0f, -1.0f, 0.0f};
        const Vec3 towardsWall = Vec3{0.3f, 0.2f, -1.0f}.normalized();
        const ParallaxLookup onWall = searchProbe(shiny, towardsWall, far, doorwayWall);
        const Vec3 wallHit = shiny + towardsWall * (5.0f / -towardsWall.z);
        assert(nearVec(onWall.direction, wallHit.normalized(), 2e-3f) && onWall.confidence > 0.9f);

        const ParallaxLookup onSky = searchProbe(shiny, Vec3{0.0f, 0.6f, 0.8f}, far, doorwayWall);
        assert(nearVec(onSky.direction, Vec3{0.0f, 0.6f, 0.8f}, 0.02f) && onSky.confidence > 0.9f);

        // Beside the doorway: the probe sees through it in the ray's own direction (x/z = 0.05), but the ray, starting
        // 3 m to the side, meets the wall at x = 3.25.
        const Vec3 beside{3.0f, 0.0f, 0.0f};
        const Vec3 nearlyStraight = Vec3{0.05f, 0.0f, -1.0f}.normalized();
        const ParallaxLookup besideDoorway = searchProbe(beside, nearlyStraight, far, doorwayWall);
        assert(nearVec(besideDoorway.direction, Vec3{3.25f, 0.0f, -5.0f}.normalized(), 2e-3f) && besideDoorway.confidence > 0.9f);

        // Through the doorway to the back wall.
        const ParallaxLookup throughDoorway = searchProbe(Vec3{0.2f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, far, doorwayWall);
        assert(nearVec(throughDoorway.direction, Vec3{0.2f, 0.0f, -10.0f}.normalized(), 2e-3f) && throughDoorway.confidence > 0.9f);

        // Everything the probe saw is nearer than any point of the ray: nothing to find.
        const ParallaxLookup hidden = searchProbe(Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, far, [](const Vec3&) { return 2.0f; });
        assert(hidden.confidence == 0.0f);

        /*
        * Behind an edge the probe cannot see past: a ray through the doorway at a slant, on into the back room behind
        * the wall, where the probe sees only the wall's near face. It goes behind what the probe saw where the probe's
        * line of sight leaves the doorway: the distances on either side jump from the back wall to the wall, so the
        * crossing is no surface and must not be trusted.
        */
        const ParallaxLookup acrossEdge = searchProbe(Vec3{0.0f, 0.0f, -4.9f}, Vec3{1.0f, 0.0f, -1.0f}.normalized(), far, doorwayWall);
        assert(acrossEdge.confidence < 0.1f);
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
    testSearchProbe();
    testLevels();
    testPrefilter();
    testBlendedStandIn();
}
