#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "core/dither.h"
#include "core/srgb.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("dither_test");

    // Values across 0..1 and either side of the curve's switch from the straight line to the power, three per case.
    std::vector<GpuVec4> curveInputs(float switchPoint) {
        std::vector<GpuVec4> inputs;

        for (int i = 0; i <= 100; ++i) {
            const float t = static_cast<float>(i) / 100.0f;
            inputs.push_back(gpuVec4(t, t * t * t, switchPoint * (0.5f + t)));
        }

        return inputs;
    }

    // The GPU computes three channels at once; the CPU mirror one.
    void checkCurve(GpuFunctionRunner& runner, GpuCheckReport& report, std::uint32_t function, const char* name,
                    float switchPoint, float (*mirror)(float)) {
        const std::vector<GpuVec4> inputs = curveInputs(switchPoint);
        const std::vector<GpuVec4> outputs = runner.run(shader, function, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            for (int channel = 0; channel < 3; ++channel) {
                report.check(name, transcendentalTolerance, mirror(inputs[i][channel]), outputs[i][channel]);
            }
        }
    }

    /*
    * The noise at pixel centres across a 4K screen. It is the fraction of a large product, so it keeps only the last
    * digits of its argument: at x = 3840 the inner product is about 260, where one float step is 3e-5, and multiplied
    * by 53 that becomes a difference of about 2e-3 in the result. That is far below what anyone could see in the
    * grain, but not rounding-sized, hence its own tolerance. Being a fraction, it also wraps: 0.9999 on one side and
    * 0.0001 on the other are neighbours, so the difference is measured around the circle.
    */
    void checkNoise(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{19};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 3840.0f)) + 0.5f, std::floor(random.between(0.0f, 2160.0f)) + 0.5f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_INTERLEAVED_GRADIENT_NOISE, inputs, 1, 1);
        const FloatTolerance noiseTolerance{.absolute = 4e-3f, .relative = 0.0f};

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const float expected = interleavedGradientNoise(inputs[i][0], inputs[i][1]);
            const float difference = std::abs(outputs[i][0] - expected);
            report.check("interleavedGradientNoise", noiseTolerance, 0.0f, std::min(difference, 1.0f - difference));
        }
    }

    // Dark to bright values with noise from 0 to 1 (the same noise given to both sides). Inputs: (linear rgb, noise).
    void checkDither(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{23};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float scale = i < 150 ? 0.02f : 1.2f;
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}, random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DITHER_FOR_EIGHT_BITS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            for (int channel = 0; channel < 3; ++channel) {
                report.check("ditherForEightBits", transcendentalTolerance, ditherForEightBits(inputs[i][channel], inputs[i][3]), outputs[i][channel]);
            }
        }
    }
}

void gpuTestDither(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("dither.glsl");
    checkCurve(runner, report, GPU_TEST_LINEAR_TO_SRGB, "linearToSrgb", 0.0031308f, linearToSrgb);
    checkCurve(runner, report, GPU_TEST_SRGB_TO_LINEAR, "srgbToLinear", 0.04045f, srgbToLinear);
    checkNoise(runner, report);
    checkDither(runner, report);
}
