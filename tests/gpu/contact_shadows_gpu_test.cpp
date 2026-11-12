#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/contact_shadows.h"

#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("contact_shadows_test");

    /*
    * Steps across their counts with every noise, rays and surfaces from far in front to far behind (round the bias and
    * the thickness), pixels all over and off the screen, distances either side of the fades. Inputs per case: (step,
    * steps, noise, length), (ray distance, scene distance, bias, thickness), (pixel, size), (footprint, view distance,
    * fade start, fade end). Outputs: (step distance, bias, blocked, edge fade), (distance fade).
    */
    void checkContactShadows(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{419};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const int steps = 1 + i % CONTACT_MAX_STEPS;
            const float scene = random.between(0.2f, 80.0f);
            const float thickness = random.between(0.01f, 0.5f);
            inputs.push_back(GpuVec4{static_cast<float>(i % steps), static_cast<float>(steps), random.between(0.0f, 0.999f), random.between(0.05f, 2.0f)});
            inputs.push_back(GpuVec4{scene + random.between(-0.2f, 0.7f), scene, random.between(0.0f, 0.05f), thickness});
            const float width = random.between(64.0f, 2560.0f);
            const float height = random.between(64.0f, 1440.0f);
            inputs.push_back(GpuVec4{random.between(-20.0f, width + 20.0f), random.between(-20.0f, height + 20.0f), width, height});
            const float start = random.between(0.0f, 60.0f);
            inputs.push_back(GpuVec4{random.between(0.0001f, 0.01f), random.between(0.0f, 120.0f), start, i % 9 == 0 ? start : start + random.between(0.0f, 40.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CONTACT_SHADOWS, inputs, 4, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const GpuVec4* out = &outputs[i * 2];
            report.check("contactStepDistance", arithmeticTolerance, contactStepDistance(static_cast<int>(in[0][0]), static_cast<int>(in[0][1]), in[0][2], in[0][3]),
                         out[0][0]);
            report.check("contactBias", arithmeticTolerance, contactBias(in[3][0], in[3][1]), out[0][1]);
            report.check("contactBlocked", exactTolerance, contactBlocked(in[1][0], in[1][1], in[1][2], in[1][3]) ? 1.0f : 0.0f, out[0][2]);
            report.check("contactEdgeFade", arithmeticTolerance, contactEdgeFade(Vec2{in[2][0], in[2][1]}, Vec2{in[2][2], in[2][3]}), out[0][3]);
            report.check("contactDistanceFade", arithmeticTolerance, contactDistanceFade(in[3][1], in[3][2], in[3][3]), out[1][0]);
        }
    }
}

void gpuTestContactShadows(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("contact_shadows.glsl");
    checkContactShadows(runner, report);
}