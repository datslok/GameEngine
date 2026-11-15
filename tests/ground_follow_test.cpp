#include "ecs/world.h"
#include "math/transform.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/ray_tracing.h"
#include "scene/frame_description.h"
#include "scene/ground_follow.h"
#include "scene/model_renderer.h"
#include "scene/terrain_heights.h"
#include "test_jobs.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    float plane(float x, float z) {
        return 0.5f * x + 0.25f * z + 3.0f;
    }

    float waves(float x, float z) {
        return 4.0f * std::sin(0.3f * x) * std::cos(0.2f * z) + 0.1f * x;
    }

    // A fine grid 64 m across round the origin at 1 m, a coarse one 1 km across at 16 m, as the terrain's tests have.
    TerrainHeights heightsOf(float (*height)(float, float)) {
        TerrainHeights heights;
        heights.grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, 0.0f}, 32.0f, 1.0f, height, testJobs()));
        heights.grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, 0.0f}, 512.0f, 16.0f, height, testJobs()));
        return heights;
    }

    // A point lifted onto the land is raised by the land's height at its own place, as the vertex shaders raise it.
    void testAPointIsLiftedByTheLandUnderIt() {
        const TerrainHeights heights = heightsOf(waves);
        const Vec3 lifted = liftedOntoGround(heights, Vec3{3.5f, 0.4f, -2.25f});
        assert(lifted.x == 3.5f && lifted.z == -2.25f);
        assert(near(lifted.y, 0.4f + terrainHeight(heights, 3.5f, -2.25f), 1e-5f));

        // Without any land, nothing moves (the shaders' terrainHeight reads 0 with no grids).
        assert(liftedOntoGround(TerrainHeights{}, Vec3{1.0f, 2.0f, 3.0f}).y == 2.0f);
    }

    /*
    * A sphere lifted onto the land covers everything in it lifted: moved up by the middle of the land's heights under its
    * square, grown by half their range. On a plane that is exact: the middle is the height under its centre.
    */
    void testASphereCoversItsContentsLifted() {
        const TerrainHeights tilted = heightsOf(plane);
        const BoundingSphere sphere{Vec3{4.0f, 0.5f, -3.0f}, 6.0f};
        const BoundingSphere onPlane = liftedSphere(sphere, tilted);
        // Across the square the plane rises 0.5 * 12 + 0.25 * 12 = 9 m, so the sphere grows by 4.5 m.
        assert(near(onPlane.centre.y, 0.5f + plane(4.0f, -3.0f), 1e-3f) && near(onPlane.radius, 6.0f + 4.5f, 1e-3f));
        assert(onPlane.centre.x == 4.0f && onPlane.centre.z == -3.0f);

        // On wavy land, points all through the sphere, each lifted by its own height, stay inside.
        const TerrainHeights wavy = heightsOf(waves);
        const BoundingSphere onWaves = liftedSphere(sphere, wavy);

        for (int i = -6; i <= 6; ++i) {
            for (int j = -6; j <= 6; ++j) {
                for (int k = -6; k <= 6; ++k) {
                    const Vec3 offset{static_cast<float>(i) * 0.97f, static_cast<float>(j) * 0.97f, static_cast<float>(k) * 0.97f};

                    if (offset.length() > sphere.radius) {
                        continue;
                    }

                    const Vec3 lifted = liftedOntoGround(wavy, sphere.centre + offset);
                    assert((lifted - onWaves.centre).length() <= onWaves.radius + 1e-3f);
                }
            }
        }

        // An unknown size stays unknown.
        assert(liftedSphere(BoundingSphere{}, wavy).radius < 0.0f);
    }

    /*
    * The engine lifts the spheres of draws following the ground (their bounds and the ones their levels are chosen by),
    * and only theirs; without land, none.
    */
    void testOnlyGroundDrawsAreLifted() {
        const TerrainHeights heights = heightsOf(plane);
        std::vector<DrawItem> draws(2);

        for (DrawItem& draw : draws) {
            draw.bounds = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
            draw.levelBounds = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 2.0f};
        }

        draws[1].followsGround = true;
        liftGroundDraws(draws, &heights);
        assert(draws[0].bounds.centre.y == 0.0f && draws[0].levelBounds.centre.y == 0.0f);
        assert(near(draws[1].bounds.centre.y, plane(0.0f, 0.0f), 1e-3f) && near(draws[1].levelBounds.centre.y, plane(0.0f, 0.0f), 1e-3f));
        assert(draws[1].levelBounds.radius > 2.0f);

        std::vector<DrawItem> without(1);
        without[0].followsGround = true;
        without[0].bounds = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
        liftGroundDraws(without, nullptr);
        assert(without[0].bounds.centre.y == 0.0f && without[0].bounds.radius == 1.0f);
    }

    /*
    * A model renderer following the ground makes draws that do: their instances say so to the vertex shaders (an
    * impostor's card stays one), and no ray meets them, since rays would meet the mesh where it stands, not lifted.
    */
    void testTheFlagReachesTheShaders() {
        World world;
        const Entity entity = world.create();
        world.add(entity, Transform{});
        ModelRenderer renderer;
        renderer.parts.push_back(RenderPart{MeshHandle{0}, Material{}});
        renderer.followsGround = true;
        world.add(entity, renderer);
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        const FrameDescription frame = buildFrame(world, camera, 1.0f);
        assert(frame.draws.size() == 1 && frame.draws.front().followsGround);

        DrawItem draw;
        draw.followsGround = true;
        assert(packInstance(draw, 0).material[3] == static_cast<std::uint32_t>(INSTANCE_KIND_FOLLOWS_GROUND));
        assert(rayMaskFor(draw) == 0u);

        draw.followsGround = false;
        assert(packInstance(draw, 0).material[3] == static_cast<std::uint32_t>(INSTANCE_KIND_PLAIN) && rayMaskFor(draw) != 0u);
        draw.material.impostorFrames = 8;
        assert(packInstance(draw, 0).material[3] == static_cast<std::uint32_t>(INSTANCE_KIND_IMPOSTOR));
    }
}

void testGroundFollow() {
    testAPointIsLiftedByTheLandUnderIt();
    testASphereCoversItsContentsLifted();
    testOnlyGroundDrawsAreLifted();
    testTheFlagReachesTheShaders();
}
