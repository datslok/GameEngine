#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/ground_layers.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("ground_layers_test");

    /*
    * Painted weights from none at all to all four, some layers unpainted (which must never show), heights tied and
    * apart, and blend depths from 0 (the painted weights alone) to past the heights' range (the plain cross-fade).
    * Inputs per case: painted weights, heights, (blend depth, 0, 0, 0).
    */
    void checkBlendWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            GpuVec4 painted{};
            GpuVec4 heights{};

            for (std::size_t layer = 0; layer < 4; ++layer) {
                painted[layer] = (i + static_cast<int>(layer)) % 3 == 0 ? 0.0f : random.between(0.0f, 1.0f);
                heights[layer] = i % 7 == 0 ? 0.5f : random.between(0.0f, 1.0f);
            }

            if (i % 25 == 0) {
                painted = GpuVec4{};
            }

            const float depth = i % 11 == 0 ? 0.0f : random.between(0.0f, 1.5f);
            inputs.push_back(painted);
            inputs.push_back(heights);
            inputs.push_back(GpuVec4{depth, 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GROUND_BLEND_WEIGHTS, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GroundWeights expected = groundBlendWeights(GroundWeights{in[0][0], in[0][1], in[0][2], in[0][3]},
                                                              GroundWeights{in[1][0], in[1][1], in[1][2], in[1][3]}, in[2][0]);

            for (std::size_t layer = 0; layer < 4; ++layer) {
                report.check("groundBlendWeights", arithmeticTolerance, expected[layer], outputs[i][layer]);
            }
        }
    }
}

void gpuTestGroundLayers(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ground_layers.glsl");
    checkBlendWeights(runner, report);
}
