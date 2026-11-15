#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/terrain_heights.h"

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

void gpuTestTerrain(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("terrain.glsl");
    checkMorph(runner, report);
    checkMorphGrid(runner, report);
    checkBilinear(runner, report);
}
