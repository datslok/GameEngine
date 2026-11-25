#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/terrain_heights.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("terrain_test");

    // Distances before, through and past morphs of many lengths.
    void checkMorph(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float start = random.between(0.0f, 2000.0f);
            const float end = start + random.between(1.0f, 500.0f);
            inputs.push_back(gpuVec4(random.between(start - 200.0f, end + 200.0f), start, end));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TERRAIN_MORPH, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("terrainMorph", arithmeticTolerance, terrainMorph(inputs[i][0], inputs[i][1], inputs[i][2]), outputs[i][0]);
        }
    }

    // Whole grid coordinates, odd and even, slid by morphs from 0 to 1: exact, the slide a power of two's multiple.
    void checkMorphGrid(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 33.0f)), std::floor(random.between(0.0f, 33.0f)), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TERRAIN_MORPH_GRID, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Vec2 expected = morphGridCoordinate(Vec2{inputs[i][0], inputs[i][1]}, inputs[i][2]);
            report.check("morphGridCoordinate", arithmeticTolerance, expected.x, outputs[i][0]);
            report.check("morphGridCoordinate", arithmeticTolerance, expected.y, outputs[i][1]);
        }
    }

    // Four heights of a valley's range and a place in their square. Inputs: the heights, then (across, down).
    void checkBilinear(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{47};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-50.0f, 300.0f), random.between(-50.0f, 300.0f), random.between(-50.0f, 300.0f),
                                     random.between(-50.0f, 300.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TERRAIN_BILINEAR, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& heights = inputs[2 * i];
            const GpuVec4& place = inputs[2 * i + 1];
            report.check("terrainBilinear", arithmeticTolerance, terrainBilinear(heights[0], heights[1], heights[2], heights[3], place[0], place[1]),
                         outputs[i][0]);
        }
    }
}

namespace {
    // The same cases for the drawn square's triangles, places on both sides of the diagonal.
    void checkTriangle(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-50.0f, 300.0f), random.between(-50.0f, 300.0f), random.between(-50.0f, 300.0f),
                                     random.between(-50.0f, 300.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TERRAIN_TRIANGLE, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& heights = inputs[2 * i];
            const GpuVec4& place = inputs[2 * i + 1];
            report.check("terrainTriangleHeight", arithmeticTolerance,
                         terrainTriangleHeight(heights[0], heights[1], heights[2], heights[3], place[0], place[1]), outputs[i][0]);
        }
    }
}

namespace {
    /*
    * Patches of every level's size, full and half grids, morph spacings, and footprints of whole and odd grid points:
    * terrainClusterBox's box exactly (whole numbers of spacings), and terrainBoxSphere round boxes far and near the
    * origin, to rounding.
    */
    void checkClusterBounds(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{59};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float quads = i % 2 == 0 ? 32.0f : 16.0f;
            const float size = 64.0f * std::exp2(std::floor(random.between(0.0f, 7.0f)));
            const float low = std::floor(random.between(0.0f, quads - 1.0f)) / quads;
            const float lowZ = std::floor(random.between(0.0f, quads - 1.0f)) / quads;
            inputs.push_back(gpuVec4(std::floor(random.between(-40.0f, 40.0f)) * 64.0f, std::floor(random.between(-40.0f, 40.0f)) * 64.0f, size, quads));
            inputs.push_back(gpuVec4(100.0f, 200.0f, size / quads));
            inputs.push_back(gpuVec4(low, lowZ, std::min(low + random.between(0.0f, 0.5f), 1.0f), std::min(lowZ + random.between(0.0f, 0.5f), 1.0f)));
        }

        const std::vector<GpuVec4> boxes = runner.run(shader, GPU_TEST_TERRAIN_CLUSTER_BOX, inputs, 3, 1);

        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const GpuVec4& placement = inputs[3 * i];
            const GpuVec4& footprint = inputs[3 * i + 2];
            const TerrainPatch patch{.corner = Vec2{placement[0], placement[1]}, .size = placement[2], .quads = placement[3], .spacing = inputs[3 * i + 1][2]};
            const Vec4 expected = terrainClusterBox(patch, Vec4{footprint[0], footprint[1], footprint[2], footprint[3]});
            report.check("terrainClusterBox", exactTolerance, Vec3{expected.x, expected.y, expected.z}, Vec3{boxes[i][0], boxes[i][1], boxes[i][2]});
            report.check("terrainClusterBox", exactTolerance, expected.w, boxes[i][3]);
        }

        std::vector<GpuVec4> spheres;

        for (int i = 0; i < 500; ++i) {
            const float west = random.between(-3000.0f, 3000.0f);
            const float north = random.between(-3000.0f, 3000.0f);
            const float lowest = random.between(-50.0f, 300.0f);
            spheres.push_back(gpuVec4(west, north, west + random.between(1.0f, 500.0f), north + random.between(1.0f, 500.0f)));
            spheres.push_back(gpuVec4(lowest, lowest + random.between(0.0f, 200.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TERRAIN_BOX_SPHERE, spheres, 2, 1);
        const FloatTolerance place{.absolute = 1e-3f, .relative = 1e-5f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& box = spheres[2 * i];
            const Vec4 expected = terrainBoxSphere(Vec4{box[0], box[1], box[2], box[3]}, TerrainHeightRange{spheres[2 * i + 1][0], spheres[2 * i + 1][1]});
            report.check("terrainBoxSphere", place, Vec3{expected.x, expected.y, expected.z}, Vec3{outputs[i][0], outputs[i][1], outputs[i][2]});
            report.check("terrainBoxSphere radius", place, expected.w, outputs[i][3]);
        }
    }
}

void gpuTestTerrain(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("terrain.glsl");
    checkMorph(runner, report);
    checkMorphGrid(runner, report);
    checkBilinear(runner, report);
    checkTriangle(runner, report);
    checkClusterBounds(runner, report);
}
