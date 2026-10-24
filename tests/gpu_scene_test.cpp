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
        assert(sameFloats(packShadowView(tile).matrix, expected, 16));
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
    // The cascades' blend band and whether to dither between them instead reach the shader through the frame's block.
    void testFrameDataCarriesTheCascadeTransition() {
        const Camera camera{Vec3{1.0f, 2.0f, 3.0f}, Vec3{-4.0f, 0.0f, -2.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.1f, 1.6f, 0.1f, 100.0f};
        const ClusterGrid grid = makeClusterGrid(camera, 1280, 800);

        ShadowSettings shadows;
        shadows.cascadeBlendBand = 0.2f;
        const GpuFrameData blended = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(blended.cascadeOptions[0] == 0.2f && blended.cascadeOptions[1] == 0.0f);

        shadows.cascadeTransition = CascadeTransition::Dither;
        const GpuFrameData dithered = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(dithered.cascadeOptions[1] == 1.0f);

        // The atlas grows for Ultra cascades, and with it the size of one of its texels.
        shadows.cascadeResolution = CascadeResolution::Pixels4096;
        const GpuFrameData ultra = makeFrameData(GpuLights{}, Mat4::identity(), camera.getPosition(), grid, true, false, shadows);
        assert(ultra.atlasTexel[0] == 1.0f / 8192.0f && ultra.atlasTexel[1] == 1.0f / 12288.0f);
    }
}

void testGpuScene() {
    testFrameDataCarriesTheCascadeTransition();
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
    testIdenticalMaterialsShareAnEntry();
    testTexturesTellMaterialsApart();
    testFrameDataCarriesCameraLightsAndAtlasTexel();
    testFrameDataCarriesTheClusterGrid();
}
