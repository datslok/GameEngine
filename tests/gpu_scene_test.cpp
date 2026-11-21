#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/clusters.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/camera.h"
#include "scene/frame_description.h"

#include "test_jobs.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
    bool near(float actual, float expected) {
        return std::abs(actual - expected) < 1e-5f;
    }

    DrawItem drawOf(std::uint32_t mesh, float x = 0.0f) {
        DrawItem draw;
        draw.mesh = MeshHandle{mesh};
        draw.model = Mat4::translation(x, 0.0f, 0.0f);
        return draw;
    }

    bool sameBatch(const DrawBatch& batch, std::uint32_t mesh, std::uint32_t firstInstance, std::uint32_t instanceCount) {
        return batch.mesh == MeshHandle{mesh} && batch.firstInstance == firstInstance && batch.instanceCount == instanceCount;
    }

    bool sameFloats(const float* actual, const float* expected, std::size_t count) {
        return std::memcmp(actual, expected, count * sizeof(float)) == 0;
    }

    // Every draw of the same mesh becomes one instanced draw: the meshes in order, and within each the draws in the order
    // they were chosen (the order the instances are written in, so instance i of a batch is order[first + i]).
    void testDrawsAreBatchedByMesh() {
        const std::vector<DrawItem> draws{drawOf(2), drawOf(0), drawOf(2), drawOf(1), drawOf(0)};
        const std::vector<std::size_t> chosen{0, 1, 2, 3, 4};

        const BatchedDraws batched = batchByMesh(draws, chosen);

        assert((batched.order == std::vector<std::size_t>{1, 4, 3, 0, 2}));
        assert(batched.batches.size() == 3);
        assert(sameBatch(batched.batches[0], 0, 0, 2));
        assert(sameBatch(batched.batches[1], 1, 2, 1));
        assert(sameBatch(batched.batches[2], 2, 3, 2));
    }

    // Only the chosen draws (those a view can see) are batched, and draws of one mesh keep the order they were chosen in,
    // so the same frame always gives the same instances.
    void testOnlyChosenDrawsAreBatchedInTheirOrder() {
        const std::vector<DrawItem> draws{drawOf(5), drawOf(5), drawOf(3), drawOf(5)};
        const std::vector<std::size_t> chosen{3, 0, 2};

        const BatchedDraws batched = batchByMesh(draws, chosen);

        assert((batched.order == std::vector<std::size_t>{2, 3, 0}));
        assert(batched.batches.size() == 2);
        assert(sameBatch(batched.batches[0], 3, 0, 1));
        assert(sameBatch(batched.batches[1], 5, 1, 2));
    }

    // Shadow tiles write their instances one after another into one buffer, so each tile's batches start where the last
    // tile's ended.
    void testBatchesStartAtTheFirstInstance() {
        const std::vector<DrawItem> draws{drawOf(1), drawOf(0), drawOf(1)};

        const BatchedDraws batched = batchByMesh(draws, {0, 1, 2}, 10);

        assert(sameBatch(batched.batches[0], 0, 10, 1));
        assert(sameBatch(batched.batches[1], 1, 11, 2));
    }

    /*
    * Cut-outs and double-sided draws are drawn differently (a pipeline that throws away the holes, no back faces
    * culled), so draws of one mesh split by how they are drawn: plain draws first, then double-sided, cut-outs, and
    * double-sided cut-outs, each by mesh, and each batch says how it is drawn.
    */
    void testDrawsAreBatchedByStyle() {
        DrawItem leaf = drawOf(0);
        leaf.material.alphaMode = AlphaMode::Mask;
        leaf.material.doubleSided = true;
        DrawItem sheet = drawOf(0);
        sheet.material.doubleSided = true;
        DrawItem grate = drawOf(1);
        grate.material.alphaMode = AlphaMode::Mask;

        const std::vector<DrawItem> draws{leaf, drawOf(1), grate, sheet, drawOf(0), leaf};
        const BatchedDraws batched = batchByMesh(draws, {0, 1, 2, 3, 4, 5});

        assert((batched.order == std::vector<std::size_t>{4, 1, 3, 2, 0, 5}));
        assert(batched.batches.size() == 5);
        assert(sameBatch(batched.batches[0], 0, 0, 1) && !batched.batches[0].cutOut && !batched.batches[0].doubleSided);
        assert(sameBatch(batched.batches[1], 1, 1, 1) && !batched.batches[1].cutOut);
        assert(sameBatch(batched.batches[2], 0, 2, 1) && !batched.batches[2].cutOut && batched.batches[2].doubleSided);
        assert(sameBatch(batched.batches[3], 1, 3, 1) && batched.batches[3].cutOut && !batched.batches[3].doubleSided);
        assert(sameBatch(batched.batches[4], 0, 4, 2) && batched.batches[4].cutOut && batched.batches[4].doubleSided);
    }

    void testNothingChosenGivesNoBatches() {
        const BatchedDraws batched = batchByMesh({drawOf(0)}, {});

        assert(batched.order.empty() && batched.batches.empty());
    }
}

namespace {
    // One instance per object in the frame, the same for every view: its world model matrix, the normal matrix and the
    // handedness (column-major, as GLSL reads them), and the number of its material in the frame's material table. The
    // views' own matrices are multiplied in on the GPU: the camera's from the frame's uniform block, a shadow tile's
    // pushed per tile.
    void testInstanceHoldsTheWorldMatrices() {
        DrawItem draw = drawOf(0);
        draw.model = Mat4::translation(1000.0f, 2.0f, -3.0f) * Mat4::rotationY(0.4f) * Mat4::scaling(1.0f, 2.0f, 0.5f);

        const GpuInstance instance = packInstance(draw, 7);

        float expected[16]{};

        writeColumnMajor(draw.model, expected);
        assert(sameFloats(instance.model, expected, 16));

        writeColumnMajor(normalMatrix(draw.model), expected);
        assert(sameFloats(instance.normalMatrix, expected, 16));

        assert(instance.handedness[0] == 1.0f);
        assert(instance.material[0] == 7);

        // The mesh's number, so a ray that hits the instance can find the triangle's corners.
        assert(instance.material[1] == draw.mesh.index);
        assert(packInstance(drawOf(41), 7).material[1] == 41);
    }

    // A mirrored object turns its tangent frame inside out, which the vertex shader undoes with the handedness.
    void testMirroredInstanceIsLeftHanded() {
        DrawItem draw = drawOf(0);
        draw.model = Mat4::scaling(-1.0f, 1.0f, 1.0f);

        const GpuInstance instance = packInstance(draw, 0);

        assert(instance.handedness[0] == -1.0f);
        assert(near(instance.handedness[1], 1.0f));
    }

    /*
    * The object's size against its model (the cube root of how much it scales volume), for lengths a material gives in
    * model units: thick glass's thickness (KHR_materials_volume).
    */
    void testInstanceCarriesItsScale() {
        DrawItem draw = drawOf(0);
        draw.model = Mat4::translation(5.0f, 0.0f, 0.0f) * Mat4::scaling(2.0f, 2.0f, 2.0f);
        assert(near(packInstance(draw, 0).handedness[1], 2.0f));

        draw.model = Mat4::scaling(1.0f, 8.0f, 1.0f);
        assert(near(packInstance(draw, 0).handedness[1], 2.0f));
        assert(near(packInstance(drawOf(0), 0).handedness[1], 1.0f));
    }

    // A shadow tile's view is pushed column-major, and stays in world space like the instances it multiplies.
    void testShadowViewIsColumnMajor() {
        const Mat4 tile = pointShadowFaceMatrix(Vec3{1.0f, 2.0f, 3.0f}, 2, 20.0f);

        float expected[16]{};
        writeColumnMajor(tile, expected);
        const Vec4 plane{0.0f, 0.0f, 1.0f, 4.998f};
        const GpuShadowView view = packShadowView(tile, plane, Vec4{1.0f, 2.0f, 3.0f, 1.0f});
        assert(sameFloats(view.matrix, expected, 16));
        // Followed by the plane it clips at (an image light's mirror, render/gpu/shadow_map.h).
        assert(view.clipPlane[2] == 1.0f && view.clipPlane[3] == 4.998f);
        // And where its light is (the coloured shadow map's glass pass): a position, or with w 0 a direction.
        assert(view.lightPlace[0] == 1.0f && view.lightPlace[1] == 2.0f && view.lightPlace[2] == 3.0f && view.lightPlace[3] == 1.0f);
    }

    // A capture's view (a cube face's or a mirror's) is pushed the same way, followed by the plane it clips at and the
    // eye it is seen from (from the camera): the camera itself unless given.
    void testCaptureViewCarriesItsClipPlane() {
        const Mat4 view = pointShadowFaceMatrix(Vec3{1.0f, 2.0f, 3.0f}, 4, 50.0f);

        float expected[16]{};
        writeColumnMajor(view, expected);
        const GpuCaptureView packed = packCaptureView(view, Vec4{0.0f, 0.6f, 0.8f, -2.5f});
        assert(sameFloats(packed.viewProjection, expected, 16));
        assert(packed.clipPlane[0] == 0.0f && packed.clipPlane[1] == 0.6f && packed.clipPlane[2] == 0.8f && packed.clipPlane[3] == -2.5f);
        assert(packed.eye[0] == 0.0f && packed.eye[1] == 0.0f && packed.eye[2] == 0.0f);

        const GpuCaptureView seen = packCaptureView(view, Vec4{0.0f, 0.6f, 0.8f, -2.5f}, Vec3{0.0f, 0.0f, 6.0f});
        assert(seen.eye[0] == 0.0f && seen.eye[1] == 0.0f && seen.eye[2] == 6.0f);
    }

    // The vertex shader makes the model matrix camera-relative by subtracting the camera from its translation column,
    // before any multiplication (render/gpu/camera_relative.h): for a model matrix (last row 0 0 0 1) that is exactly
    // what relativeTo does, so the small numbers stay as precise as on the CPU.
    void testShaderSubtractionMatchesRelativeTo() {
        const Mat4 model = Mat4::translation(12000.5f, -3.25f, 800.0f) * Mat4::rotationZ(0.7f) * Mat4::scaling(2.0f, 1.0f, 3.0f);
        const Vec3 camera{11990.0f, 1.5f, 790.25f};

        Mat4 shaderStyle = model;
        shaderStyle.values[0][3] -= camera.x;
        shaderStyle.values[1][3] -= camera.y;
        shaderStyle.values[2][3] -= camera.z;

        const Mat4 relative = relativeTo(model, camera);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(shaderStyle.values[row][column] == relative.values[row][column]);
            }
        }
    }
}

namespace {
    // Draws carry their materials by value; the frame's table keeps one entry per different material, so a hundred
    // pillars of the same stone share one.
    void testIdenticalMaterialsShareAnEntry() {
        Material stone;
        stone.colour = Pixel{120, 110, 100};

        Material gold;
        gold.metallic = 1.0f;
        gold.roughness = 0.2f;

        MaterialTable table;

        assert(table.add(stone) == 0);
        assert(table.add(gold) == 1);
        assert(table.add(stone) == 0);
        assert(table.add(Material{stone}) == 0);
        assert(table.getEntries().size() == 2);

        // Entries are exactly what packMaterial makes.
        const GpuMaterial packed = packMaterial(gold);
        assert(std::memcmp(&table.getEntries()[1], &packed, sizeof(GpuMaterial)) == 0);
    }

    // Materials that differ only in a texture are different entries.
    void testTexturesTellMaterialsApart() {
        Material plain;
        Material textured;
        textured.texture = TextureHandle{4};

        MaterialTable table;

        assert(table.add(plain) == 0);
        assert(table.add(textured) == 1);
    }

    // The frame's small uniform block: the camera's view-projection measured from the camera and where the camera is (the
    // vertex shader makes each instance camera-relative with them), the ambient light and light counts from the packed
    // lights, and the size of one shadow atlas texel.
    void testFrameDataCarriesCameraLightsAndAtlasTexel() {
        FrameLighting lighting;
        lighting.ambient = Vec3{0.1f, 0.2f, 0.3f};
        lighting.directionalLights.push_back(DirectionalLight{});
        lighting.pointLights.push_back(PlacedPointLight{Vec3{0.0f, 0.0f, 0.0f}, PointLight{}});
        lighting.pointLights.push_back(PlacedPointLight{Vec3{1.0f, 0.0f, 0.0f}, PointLight{}});

        const Mat4 relativeViewProjection = Mat4::perspective(1.2f, 1.5f, 0.1f, 100.0f) * Mat4::rotationX(0.2f);
        const Vec3 cameraOrigin{998.0f, 1.0f, -4.0f};
        const Camera camera{cameraOrigin, cameraOrigin + Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 1.5f, 0.1f, 100.0f};
        const GpuFrameData frame = makeFrameData(packLighting(lighting, cameraOrigin), relativeViewProjection, cameraOrigin,
                                                 makeClusterGrid(camera, 1200, 800), true, false);

        float expected[16]{};
        writeColumnMajor(relativeViewProjection, expected);
        assert(sameFloats(frame.relativeViewProjection, expected, 16));
        assert(frame.cameraOrigin[0] == 998.0f && frame.cameraOrigin[1] == 1.0f && frame.cameraOrigin[2] == -4.0f);


        assert(frame.ambient[0] == 0.1f && frame.ambient[1] == 0.2f && frame.ambient[2] == 0.3f);
        assert(frame.counts[0] == 1 && frame.counts[1] == 2 && frame.counts[2] == 0);
        // One texel of the atlas the default settings need.
        const ShadowAtlasSize atlas = shadowAtlasSizeFor(ShadowSettings{});
        assert(frame.atlasTexel[0] == 1.0f / static_cast<float>(atlas.width));
        assert(frame.atlasTexel[1] == 1.0f / static_cast<float>(atlas.height));
    }

    /*
    * The cluster grid reaches the shaders in the same block, laid out as clusters.glsl's ClusterGrid (tile counts and
    * slices, the screen and lens, the slicing, the view's axes), and the shading options: whether the main pass shades
    * from the clusters' lists or loops over every light, and whether it shows the clusters' light counts instead.
    */
    void testFrameDataCarriesTheClusterGrid() {
        const Camera camera{Vec3{1.0f, 2.0f, 3.0f}, Vec3{-4.0f, 0.0f, -2.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.1f, 1.6f, 0.1f, 100.0f};
        const ClusterGrid grid = makeClusterGrid(camera, 1280, 800);
        const GpuFrameData clustered = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false);

        assert(clustered.clusterTiles[0] == 20 && clustered.clusterTiles[1] == 13 && clustered.clusterTiles[2] == clusterDepthSlices);
        assert(clustered.shadingOptions[0] == 1 && clustered.shadingOptions[1] == 0);
        assert(clustered.clusterScreen[0] == 1280.0f && clustered.clusterScreen[1] == 800.0f);
        assert(clustered.clusterScreen[2] == grid.tanHalfWidth && clustered.clusterScreen[3] == grid.tanHalfHeight);
        assert(clustered.clusterSlicing[0] == grid.sliceScale && clustered.clusterSlicing[1] == grid.sliceBias);
        assert(clustered.viewRight[0] == grid.right.x && clustered.viewRight[1] == grid.right.y && clustered.viewRight[2] == grid.right.z);
        assert(clustered.viewUp[0] == grid.up.x && clustered.viewUp[1] == grid.up.y && clustered.viewUp[2] == grid.up.z);
        assert(clustered.viewForward[0] == grid.forward.x && clustered.viewForward[1] == grid.forward.y && clustered.viewForward[2] == grid.forward.z);

        const GpuFrameData looped = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, false, true);
        assert(looped.shadingOptions[0] == 0 && looped.shadingOptions[1] == 1);
    }
    // The cascades' blend band, whether to dither between them instead and the leaves' lift reach the shader through the
    // frame's block.
    void testFrameDataCarriesTheCascadeTransition() {
        const Camera camera{Vec3{1.0f, 2.0f, 3.0f}, Vec3{-4.0f, 0.0f, -2.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.1f, 1.6f, 0.1f, 100.0f};
        const ClusterGrid grid = makeClusterGrid(camera, 1280, 800);

        ShadowSettings shadows;
        shadows.cascadeBlendBand = 0.2f;
        shadows.foliageShadowLift = 0.25f;
        const GpuFrameData blended = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(blended.cascadeOptions[0] == 0.2f && blended.cascadeOptions[1] == 0.0f && blended.cascadeOptions[3] == 0.25f);

        shadows.cascadeTransition = CascadeTransition::Dither;
        const GpuFrameData dithered = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(dithered.cascadeOptions[1] == 1.0f);

        // The atlas grows for Ultra cascades, and with it the size of one of its texels.
        shadows.cascadeResolution = CascadeResolution::Pixels4096;
        const GpuFrameData ultra = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(ultra.atlasTexel[0] == 1.0f / 8192.0f && ultra.atlasTexel[1] == 1.0f / 12288.0f);
    }

    /*
    * The main pass writes the motion vectors (motion_fill and the resolve read them): the frame's block carries last
    * frame's camera-relative view-projection without the jitter, column-major, this frame's jitter (pixels), and whether
    * to work the motion out at all (off: every pixel left at no motion). None until packed.
    */
    void testFrameDataCarriesTheMotionCameras() {
        const Camera camera{Vec3{1.0f, 2.0f, 3.0f}, Vec3{-4.0f, 0.0f, -2.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.1f, 1.6f, 0.1f, 100.0f};
        GpuFrameData frame = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), makeClusterGrid(camera, 1280, 800), true, false);
        assert(frame.motionOptions[0] == 0.0f);

        packFrameMotion(frame, Mat4::translation(0.0f, 5.0f, 0.0f), Vec2{0.25f, -0.375f}, true);
        assert(frame.motionPrevious[13] == 5.0f && frame.motionPrevious[0] == 1.0f);
        assert(frame.motionOptions[0] == 1.0f && frame.motionOptions[1] == 0.25f && frame.motionOptions[2] == -0.375f);

        packFrameMotion(frame, Mat4::identity(), Vec2{}, false);
        assert(frame.motionOptions[0] == 0.0f && frame.motionPrevious[13] == 0.0f);
    }
}

namespace {
    /*
    * Packing on every core gives exactly the serial instances and material table: each draw's instance as packInstance
    * makes it with the index MaterialTable::add gives its material, materials alternating as a forest's bark and leaves
    * do, a blended ground among them.
    */
    void testPackingOnManyCoresIsTheSame() {
        std::vector<DrawItem> draws;

        for (int i = 0; i < 3000; ++i) {
            DrawItem draw;
            draw.mesh = MeshHandle{static_cast<std::uint32_t>(i % 7)};
            draw.model = Mat4::translation(static_cast<float>(i), 1.0f, -2.0f) * Mat4::scaling(1.0f + 0.001f * static_cast<float>(i), 1.0f, 1.0f);
            draw.material.roughness = 0.25f * static_cast<float>(i % 3);
            draw.material.colour = Pixel{static_cast<std::uint8_t>(i % 2 == 0 ? 200 : 40), 120, 60};

            if (i == 1500) {
                Material layer;
                layer.roughness = 0.9f;
                draw.material.groundLayers = {layer, draw.material};
                draw.material.groundWeights = TextureHandle{1};
            }

            draws.push_back(draw);
        }

        MaterialTable serialTable;
        std::vector<GpuInstance> serial;

        for (const DrawItem& draw : draws) {
            serial.push_back(packInstance(draw, serialTable.add(draw.material)));
        }

        MaterialTable parallelTable;
        const std::vector<GpuInstance> parallel = packInstances(draws, parallelTable, testJobs());
        assert(parallel.size() == serial.size());
        assert(std::memcmp(parallel.data(), serial.data(), serial.size() * sizeof(GpuInstance)) == 0);
        assert(parallelTable.getEntries().size() == serialTable.getEntries().size() && serialTable.getEntries().size() < 20);
        assert(std::memcmp(parallelTable.getEntries().data(), serialTable.getEntries().data(), serialTable.getEntries().size() * sizeof(GpuMaterial)) == 0);
    }

    /*
    * The terrain's patches: runs of draws sharing a region's blended ground, regions alternating, grounds that differ
    * in one layer only, a strata ground with its top layer, ordinary draws between: packed on every core, the instances
    * and table are exactly what adding one by one gives (the layers before their ground). A ground packing refuses
    * stays refused.
    */
    void testGroundRunsPackAsOneByOne() {
        const auto layerOf = [](float roughness, std::uint8_t red) {
            Material layer;
            layer.roughness = roughness;
            layer.colour = Pixel{red, 100, 50};
            return layer;
        };
        const auto groundOf = [](std::vector<Material> layers, std::uint32_t weights) {
            Material ground;
            ground.groundLayers = std::move(layers);
            ground.groundWeights = TextureHandle{weights};
            return ground;
        };
        Material grass = groundOf({layerOf(0.8f, 40), layerOf(0.6f, 90), layerOf(0.9f, 120), layerOf(0.5f, 200)}, 1);
        Material rocks = groundOf({layerOf(0.8f, 40), layerOf(0.6f, 90), layerOf(0.9f, 120), layerOf(0.4f, 200)}, 1);
        Material otherWeights = groundOf({layerOf(0.8f, 40), layerOf(0.6f, 90)}, 2);
        Material strata = groundOf({layerOf(0.3f, 10), layerOf(0.7f, 30), layerOf(0.2f, 60)}, 3);
        strata.groundPaint = GroundPaint::Strata;
        strata.groundTopLayer = 2;
        strata.strataBottom = 10.0f;
        strata.strataTop = 40.0f;
        const std::array<Material, 4> regions{grass, rocks, otherWeights, strata};

        std::vector<DrawItem> draws;

        for (int i = 0; i < 2000; ++i) {
            DrawItem draw;
            draw.mesh = MeshHandle{static_cast<std::uint32_t>(i % 2)};
            draw.model = Mat4::translation(static_cast<float>(i), 0.0f, 0.0f);
            draw.material = i % 97 == 50 ? layerOf(0.1f, 7) : regions[static_cast<std::size_t>((i / 37) % 4)];
            draw.terrainPatch = i;
            draws.push_back(draw);
        }

        MaterialTable serialTable;
        std::vector<GpuInstance> serial;

        for (const DrawItem& draw : draws) {
            serial.push_back(packInstance(draw, serialTable.add(draw.material)));
        }

        MaterialTable parallelTable;
        const std::vector<GpuInstance> parallel = packInstances(draws, parallelTable, testJobs());
        assert(std::memcmp(parallel.data(), serial.data(), serial.size() * sizeof(GpuInstance)) == 0);
        assert(parallelTable.getEntries().size() == serialTable.getEntries().size());
        assert(std::memcmp(parallelTable.getEntries().data(), serialTable.getEntries().data(), serialTable.getEntries().size() * sizeof(GpuMaterial)) == 0);

        // Over a base holding some of them: the same numbers as the one-by-one table over the same base.
        MaterialTable base;
        base.add(grass);
        MaterialTable serialOver = MaterialTable::over(base);
        MaterialTable parallelOver = MaterialTable::over(base);

        for (std::size_t draw = 0; draw < draws.size(); ++draw) {
            serial[draw] = packInstance(draws[draw], serialOver.add(draws[draw].material));
        }

        const std::vector<GpuInstance> overBase = packInstances(draws, parallelOver, testJobs());
        assert(std::memcmp(overBase.data(), serial.data(), serial.size() * sizeof(GpuInstance)) == 0);
        assert(parallelOver.getEntries().size() == serialOver.getEntries().size());

        // Refused as before: too many layers, layered layers, no weights, a strata top layer past the layers; a run of
        // the same refused ground too.
        const auto refused = [](const Material& material) {
            std::vector<DrawItem> bad(3);

            for (DrawItem& draw : bad) {
                draw.material = material;
            }

            MaterialTable table;

            try {
                packInstances(bad, table, testJobs());
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };
        assert(refused(groundOf({layerOf(0.1f, 1), layerOf(0.2f, 2), layerOf(0.3f, 3), layerOf(0.4f, 4), layerOf(0.5f, 5)}, 1)));
        assert(refused(groundOf({grass}, 1)));
        Material noWeights = groundOf({layerOf(0.1f, 1)}, 1);
        noWeights.groundWeights = TextureHandle{};
        assert(refused(noWeights));
        Material badStrata = strata;
        badStrata.groundTopLayer = 5;
        assert(refused(badStrata));
    }

    /*
    * A frame's draw list laid out: the GPU's regions only counted, the CPU's entries in runs at their places, runs side
    * by side joined into one, so writing them leaves the GPU's regions untouched.
    */
    void testDrawListLayoutLeavesTheGpuRegions() {
        DrawListLayout layout;
        assert(layout.reserve(100) == 0);
        const std::vector<std::uint32_t> glass{7, 8};
        assert(layout.append(glass) == 100);
        assert(layout.append(std::vector<std::uint32_t>{9}) == 102);
        assert(layout.reserve(50) == 103);
        assert(layout.append(std::vector<std::uint32_t>{}) == 153);
        assert(layout.append(std::vector<std::uint32_t>{1, 2, 3}) == 153);
        assert(layout.size() == 156);

        const std::vector<DrawListLayout::Run>& runs = layout.getRuns();
        assert(runs.size() == 2);
        assert(runs[0].first == 100 && (runs[0].entries == std::vector<std::uint32_t>{7, 8, 9}));
        assert(runs[1].first == 153 && (runs[1].entries == std::vector<std::uint32_t>{1, 2, 3}));
    }
}

void testGpuScene() {
    testDrawListLayoutLeavesTheGpuRegions();
    testPackingOnManyCoresIsTheSame();
    testGroundRunsPackAsOneByOne();
    testFrameDataCarriesTheCascadeTransition();
    testFrameDataCarriesTheMotionCameras();
    testDrawsAreBatchedByMesh();
    testOnlyChosenDrawsAreBatchedInTheirOrder();
    testBatchesStartAtTheFirstInstance();
    testNothingChosenGivesNoBatches();
    testDrawsAreBatchedByStyle();
    testInstanceHoldsTheWorldMatrices();
    testMirroredInstanceIsLeftHanded();
    testInstanceCarriesItsScale();
    testShaderSubtractionMatchesRelativeTo();
    testShadowViewIsColumnMajor();
    testCaptureViewCarriesItsClipPlane();
    testIdenticalMaterialsShareAnEntry();
    testTexturesTellMaterialsApart();
    testFrameDataCarriesCameraLightsAndAtlasTexel();
    testFrameDataCarriesTheClusterGrid();
}
