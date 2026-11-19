#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/window_mask.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("window_mask_test");

    // Centres hundreds of metres either side of zero, footprints from a point to many cells, slits from thin to nearly full.
    void checkPulseCoverage(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{83};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float pitch = random.between(2.0f, 12.0f);
            const float footprint = i % 5 == 0 ? 0.0f : random.between(0.01f, 40.0f);
            inputs.push_back(GpuVec4{random.between(-400.0f, 400.0f), footprint, pitch, random.between(0.05f, 0.95f) * pitch});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PULSE_COVERAGE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("pulseIntegral", arithmeticTolerance, pulseIntegral(in[0], in[2], in[3]), outputs[i][0]);
            // Differences of two running covers hundreds of metres out, over a small footprint: rounding grows with both.
            report.check("pulseCoverage", FloatTolerance{.absolute = 2e-3f, .relative = 1e-4f}, pulseCoverage(in[0], in[1], in[2], in[3]), outputs[i][1]);
        }
    }

    // Cells either side of zero; facet numbers anywhere in 0..1; footprints round the far blend's edges.
    void checkHashes(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{89};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            inputs.push_back(GpuVec4{random.between(-500.0f, 500.0f), random.between(-30.0f, 800.0f), random.between(2.0f, 12.0f), 0.0f});
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 20.0f), random.between(0.0f, 2.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WINDOW_HASH, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const GpuVec4* out = &outputs[i * 2];
            const std::uint32_t seed = windowFaceSeed(Vec2{in[1][0], in[1][1]});
            const Vec3 hashed = windowHash(in[0][0], in[0][1], in[0][2], seed);
            report.check("windowFaceSeed", FloatTolerance{.absolute = 0.0f, .relative = 0.0f}, static_cast<float>(seed), out[1][0]);
            report.check("windowHash", arithmeticTolerance, hashed.x, out[0][0]);
            report.check("windowHash", arithmeticTolerance, hashed.y, out[0][1]);
            report.check("windowHash", arithmeticTolerance, hashed.z, out[0][2]);
            report.check("windowFarShare", arithmeticTolerance, windowFarShare(Vec2{in[1][2], in[1][3]}, in[0][2]), out[0][3]);
        }
    }

    // Near and far pixels, inside and outside bands, with both colours.
    void checkEmission(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{97};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            inputs.push_back(GpuVec4{random.between(-300.0f, 300.0f), random.between(0.0f, 600.0f), random.between(0.0f, 12.0f), random.between(0.0f, 3.0f)});
            inputs.push_back(GpuVec4{i % 7 == 0 ? 0.0f : random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.05f, 0.95f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(2.0f, 12.0f), 0.0f});
            inputs.push_back(GpuVec4{random.between(0.0f, 5.0f), random.between(0.0f, 5.0f), random.between(0.0f, 5.0f), 0.0f});
            inputs.push_back(GpuVec4{random.between(0.0f, 5.0f), random.between(0.0f, 5.0f), random.between(0.0f, 5.0f), 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WINDOW_EMISSION, inputs, 5, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const Vec3 light = windowEmission(Vec2{in[0][0], in[0][1]}, Vec2{in[0][2], in[0][3]}, Vec4{in[1][0], in[1][1], in[1][2], in[1][3]}, in[2][2],
                                              windowFaceSeed(Vec2{in[2][0], in[2][1]}), Vec3{in[3][0], in[3][1], in[3][2]}, Vec3{in[4][0], in[4][1], in[4][2]});
            // Built on pulseCoverage's differences, so its tolerance carries over, scaled by the colours (up to 5).
            const FloatTolerance tolerance{.absolute = 1e-2f, .relative = 1e-3f};
            report.check("windowEmission", tolerance, light.x, outputs[i][0]);
            report.check("windowEmission", tolerance, light.y, outputs[i][1]);
            report.check("windowEmission", tolerance, light.z, outputs[i][2]);
        }
    }
}

void gpuTestWindowMask(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("window_mask.glsl");
    checkPulseCoverage(runner, report);
    checkHashes(runner, report);
    checkEmission(runner, report);
}
