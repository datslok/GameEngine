#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/depth_of_field.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("depth_of_field_test");

    // Points near and far of foci near and far, focus steps short and long, samples in and out of reach in both fields,
    // every ring step, composites in and out of focus, and medians of random values. Inputs as
    // depth_of_field_test.comp lists them.
    void checkDepthOfField(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;
        constexpr std::size_t inputsPerCase = 16;

        for (int i = 0; i < 600; ++i) {
            const float focal = random.between(0.008f, 0.3f);
            const float inverseDistance = i % 7 == 0 ? 0.0f : random.between(0.0f, 10.0f);
            inputs.push_back(gpuVec4(inverseDistance, random.between(0.0f, nearestFocusInverse(focal)), focal, random.between(0.0f, 400.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 10.0f), random.between(0.0f, 10.0f), random.between(0.0f, 0.1f), i % 5 == 0 ? 0.0f : random.between(0.01f, 2.0f)));
            inputs.push_back(gpuVec4(random.between(-16.0f, 16.0f), random.between(0.0f, 17.0f), random.between(-16.0f, 16.0f), random.between(0.5f, 16.5f)));
            const int ring = 1 + i % DEPTH_OF_FIELD_RINGS;
            inputs.push_back(gpuVec4(static_cast<float>(ring), static_cast<float>((i / DEPTH_OF_FIELD_RINGS) % (8 * ring))));
            inputs.push_back(gpuVec4(random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(-3.0f, 3.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f)));

            for (int value = 0; value < 9; ++value) {
                inputs.push_back(gpuVec4(random.between(-5.0f, 5.0f), random.between(0.0f, 100.0f), random.between(0.0f, 1.0f), static_cast<float>(value % 3)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DEPTH_OF_FIELD, inputs, inputsPerCase, 4);

        for (std::size_t i = 0; i < outputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * inputsPerCase];
            const GpuVec4* out = &outputs[i * 4];
            report.check("blurRadius", arithmeticTolerance, blurRadius(in[0][0], in[0][1], in[0][2], in[0][3]), out[0][0]);
            report.check("easeFocus", transcendentalTolerance, easeFocus(in[1][0], in[1][1], in[1][2], in[1][3]), out[0][1]);
            report.check("blurCoverage", arithmeticTolerance, blurCoverage(in[2][0], in[2][1]), out[0][2]);
            const int ring = static_cast<int>(in[3][0]);
            report.check("blurRingShare", arithmeticTolerance, blurRingShare(ring), out[0][3]);

            const Vec2 offset = blurRingOffset(ring, static_cast<int>(in[3][1]));
            report.check("blurRingOffset", trigonometricTolerance(1.0f), Vec3{offset.x, offset.y, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
            report.check("farSampleWeight", arithmeticTolerance, farSampleWeight(in[2][0], in[2][2], in[2][1]), out[1][2]);
            report.check("nearSampleWeight", arithmeticTolerance, nearSampleWeight(in[2][0], in[2][1], in[2][3]), out[1][3]);

            const Vec3 composite = depthOfFieldComposite(Vec3{in[4][0], in[4][1], in[4][2]}, in[4][3], Vec3{in[5][0], in[5][1], in[5][2]},
                                                         Vec3{in[6][0], in[6][1], in[6][2]}, in[5][3]);
            report.check("depthOfFieldComposite", arithmeticTolerance, composite, Vec3{out[2][0], out[2][1], out[2][2]});
            report.check("scatterWeight", arithmeticTolerance, scatterWeight(in[2][0], in[2][1], in[2][3]), out[2][3]);

            for (int component = 0; component < 4; ++component) {
                std::array<float, 9> values{};
                for (int value = 0; value < 9; ++value) {
                    values[static_cast<std::size_t>(value)] = in[7 + value][component];
                }
                report.check("medianOfNine", exactTolerance, medianOfNine(values), out[3][component]);
            }
        }
    }
}

void gpuTestDepthOfField(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("depth_of_field.glsl");
    checkDepthOfField(runner, report);
}
