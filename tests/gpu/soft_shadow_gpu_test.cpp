#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/soft_shadow.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("soft_shadow_test");

    // Stored depths from the near plane (1, reversed-Z) to the far one (0), for short and long lights.
    void checkLinearDepth(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (float nearPlane : {0.05f, 0.1f, 0.5f}) {
            for (float farPlane : {5.0f, 20.0f, 100.0f}) {
                for (int i = 0; i <= 20; ++i) {
                    inputs.push_back(gpuVec4(static_cast<float>(i) / 20.0f, nearPlane, farPlane));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LINEAR_SHADOW_DEPTH, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("linearShadowDepth", arithmeticTolerance, linearShadowDepth(inputs[i][0], inputs[i][1], inputs[i][2]), outputs[i][0]);
        }
    }

    /*
    * Random lights and receivers, wide enough that both clamps (the smallest filter and the caps) are reached as well
    * as the range between. Inputs: (emitter radius, receiver depth, near plane or blocker depth, texel per unit).
    */
    std::vector<GpuVec4> shadowCases(std::uint32_t seed, bool blockers) {
        CaseRandom random{seed};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float receiver = random.between(0.2f, 40.0f);
            const float third = blockers ? random.between(0.05f, receiver) : random.between(0.05f, 0.5f);
            inputs.push_back(gpuVec4(random.between(0.0f, 0.5f), receiver, third, random.between(0.0005f, 0.02f)));
        }

        return inputs;
    }

    void checkBlockerSearch(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<GpuVec4> inputs = shadowCases(5, false);
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_BLOCKER_SEARCH_TEXELS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("blockerSearchTexels", arithmeticTolerance, blockerSearchTexels(in[0], in[1], in[2], in[3]), outputs[i][0]);
        }
    }

    void checkPenumbra(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<GpuVec4> inputs = shadowCases(6, true);
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PENUMBRA_TEXELS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("penumbraTexels", arithmeticTolerance, penumbraTexels(in[0], in[1], in[2], in[3]), outputs[i][0]);
        }
    }
    // Box depths over boxes of different depth: (depth, depth range).
    void checkBoxDistance(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (float range : {10.0f, 150.0f, 600.0f}) {
            for (int i = 0; i <= 20; ++i) {
                inputs.push_back(gpuVec4(static_cast<float>(i) / 20.0f, range));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_BOX_SHADOW_DISTANCE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("boxShadowDistance", arithmeticTolerance, boxShadowDistance(inputs[i][0], inputs[i][1]), outputs[i][0]);
        }
    }

    // Suns from none to a few degrees, receivers and blockers anywhere in a box, cascades' texels from 1 cm to 1 m:
    // (tan of the angular radius, receiver distance, blocker distance, texel size).
    std::vector<GpuVec4> sunCases(std::uint32_t seed) {
        CaseRandom random{seed};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float receiver = random.between(0.5f, 400.0f);
            inputs.push_back(gpuVec4(random.between(0.0f, 0.05f), receiver, random.between(0.0f, receiver), random.between(0.01f, 1.0f)));
        }

        return inputs;
    }

    void checkSunShadow(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<GpuVec4> inputs = sunCases(7);
        const std::vector<GpuVec4> search = runner.run(shader, GPU_TEST_SUN_BLOCKER_SEARCH_TEXELS, inputs, 1, 1);
        const std::vector<GpuVec4> penumbra = runner.run(shader, GPU_TEST_SUN_PENUMBRA_TEXELS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("sunBlockerSearchTexels", arithmeticTolerance, sunBlockerSearchTexels(in[0], in[1], in[3]), search[i][0]);
            report.check("sunPenumbraTexels", arithmeticTolerance, sunPenumbraTexels(in[0], in[1], in[2], in[3]), penumbra[i][0]);
        }
    }
}

void gpuTestSoftShadow(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("soft_shadow.glsl");
    checkLinearDepth(runner, report);
    checkBlockerSearch(runner, report);
    checkPenumbra(runner, report);
    checkBoxDistance(runner, report);
    checkSunShadow(runner, report);
}
