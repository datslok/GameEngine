#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/alpha_mask.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("alpha_mask_test");

    /*
    * Alphas and cutoffs across 0..1, some exactly equal (the kept edge), and every pair of facing and sidedness.
    * Inputs per case: (alpha, cutoff, front facing, double-sided). Outputs: (cut away, facing sign).
    */
    void checkAlphaMask(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{211};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float cutoff = random.between(0.0f, 1.0f);
            const float alpha = i % 8 == 0 ? cutoff : random.between(0.0f, 1.0f);
            inputs.push_back(GpuVec4{alpha, cutoff, static_cast<float>(i % 2), static_cast<float>((i / 2) % 2)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_ALPHA_MASK, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("isCutAway", exactTolerance, isCutAway(in[0], in[1]) ? 1.0f : 0.0f, outputs[i][0]);
            report.check("facingSign", exactTolerance, facingSign(in[2] != 0.0f, in[3] != 0.0f), outputs[i][1]);
        }
    }
}

void gpuTestAlphaMask(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("alpha_mask.glsl");
    checkAlphaMask(runner, report);
}
