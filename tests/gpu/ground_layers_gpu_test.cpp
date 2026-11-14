#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/ground_layers.h"

#include <cmath>
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

    // Places across texels and far out on a big texture, at and between texel centres.
    void checkPaintTaps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{47};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float spread = i % 3 == 0 ? 8.0f : 2048.0f;
            const float x = i % 10 == 0 ? std::floor(random.between(0.0f, spread)) + 0.5f : random.between(0.0f, spread);
            inputs.push_back(GpuVec4{x, random.between(0.0f, spread), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GROUND_PAINT_TAPS, inputs, 1, 2);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GroundPaintTaps expected = groundPaintTaps(Vec2{inputs[i][0], inputs[i][1]});
            const GpuVec4& places = outputs[i * 2];
            const GpuVec4& shares = outputs[i * 2 + 1];
            report.check("groundPaintTaps first", arithmeticTolerance, expected.first.x, places[0]);
            report.check("groundPaintTaps first", arithmeticTolerance, expected.first.y, places[1]);
            report.check("groundPaintTaps second", arithmeticTolerance, expected.second.x, places[2]);
            report.check("groundPaintTaps second", arithmeticTolerance, expected.second.y, places[3]);
            report.check("groundPaintTaps share", arithmeticTolerance, expected.firstShare.x, shares[0]);
            report.check("groundPaintTaps share", arithmeticTolerance, expected.firstShare.y, shares[1]);
        }
    }
}

void gpuTestGroundLayers(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ground_layers.glsl");
    checkBlendWeights(runner, report);
    checkPaintTaps(runner, report);
}
