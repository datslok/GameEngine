#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
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

    void testNothingChosenGivesNoBatches() {
        const BatchedDraws batched = batchByMesh({drawOf(0)}, {});

        assert(batched.order.empty() && batched.batches.empty());
    }
}

namespace {
    // One instance holds what each draw used to push as its vertex uniform block: the camera-relative transform and model
    // matrix, the normal matrix and the handedness, worked out exactly the same way (so the picture is unchanged bit for
    // bit), plus the number of its material in the frame's material table.
    void testInstanceHoldsTheDrawsMatrices() {
        DrawItem draw = drawOf(0);
        draw.model = Mat4::translation(1000.0f, 2.0f, -3.0f) * Mat4::rotationY(0.4f) * Mat4::scaling(1.0f, 2.0f, 0.5f);

        const Vec3 cameraOrigin{998.0f, 1.0f, 0.0f};
        const Mat4 relativeViewProjection = Mat4::perspective(1.2f, 1.5f, 0.1f, 100.0f) * Mat4::rotationX(0.2f);

        const GpuInstance instance = packInstance(draw, relativeViewProjection, cameraOrigin, 7);

        const Mat4 relativeModel = relativeTo(draw.model, cameraOrigin);
        float expected[16]{};

        writeColumnMajor(relativeViewProjection * relativeModel, expected);
        assert(sameFloats(instance.transform, expected, 16));

        writeColumnMajor(relativeModel, expected);
        assert(sameFloats(instance.model, expected, 16));

        writeColumnMajor(normalMatrix(draw.model), expected);
        assert(sameFloats(instance.normalMatrix, expected, 16));

        assert(instance.handedness[0] == 1.0f);
        assert(instance.material[0] == 7);
    }

    // A mirrored object turns its tangent frame inside out, which the vertex shader undoes with the handedness.
    void testMirroredInstanceIsLeftHanded() {
        DrawItem draw = drawOf(0);
        draw.model = Mat4::scaling(-1.0f, 1.0f, 1.0f);

        const GpuInstance instance = packInstance(draw, Mat4::identity(), Vec3{0.0f, 0.0f, 0.0f}, 0);

        assert(instance.handedness[0] == -1.0f);
    }

    // A shadow caster's instance is the tile's view-projection times the model matrix, as the shadow pass pushed it.
    void testShadowInstanceHoldsTheTileTransform() {
        const Mat4 tile = pointShadowFaceMatrix(Vec3{1.0f, 2.0f, 3.0f}, 2, 20.0f);
        const Mat4 model = Mat4::translation(4.0f, 0.0f, 1.0f) * Mat4::rotationZ(0.3f);

        const GpuShadowInstance instance = packShadowInstance(tile, model);

        float expected[16]{};
        writeColumnMajor(tile * model, expected);
        assert(sameFloats(instance.transform, expected, 16));
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

    // The frame's small uniform block: the ambient light and light counts from the packed lights, and the size of one
    // shadow atlas texel.
    void testFrameDataCarriesAmbientCountsAndAtlasTexel() {
        FrameLighting lighting;
        lighting.ambient = Vec3{0.1f, 0.2f, 0.3f};
        lighting.directionalLights.push_back(DirectionalLight{});
        lighting.pointLights.push_back(PlacedPointLight{Vec3{0.0f, 0.0f, 0.0f}, PointLight{}});
        lighting.pointLights.push_back(PlacedPointLight{Vec3{1.0f, 0.0f, 0.0f}, PointLight{}});

        const GpuFrameData frame = makeFrameData(packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f}));

        assert(frame.ambient[0] == 0.1f && frame.ambient[1] == 0.2f && frame.ambient[2] == 0.3f);
        assert(frame.counts[0] == 1 && frame.counts[1] == 2 && frame.counts[2] == 0);
        assert(frame.atlasTexel[0] == 1.0f / static_cast<float>(shadowAtlasWidth));
        assert(frame.atlasTexel[1] == 1.0f / static_cast<float>(shadowAtlasHeight));
    }
}

void testGpuScene() {
    testDrawsAreBatchedByMesh();
    testOnlyChosenDrawsAreBatchedInTheirOrder();
    testBatchesStartAtTheFirstInstance();
    testNothingChosenGivesNoBatches();
    testInstanceHoldsTheDrawsMatrices();
    testMirroredInstanceIsLeftHanded();
    testShadowInstanceHoldsTheTileTransform();
    testIdenticalMaterialsShareAnEntry();
    testTexturesTellMaterialsApart();
    testFrameDataCarriesAmbientCountsAndAtlasTexel();
}
