#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/level_fade.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("level_fade_test");

    // Shares across and past the band, both levels: integers, so exact.
    void checkFields(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (int i = 0; i <= 300; ++i) {
            inputs.push_back(gpuVec4(static_cast<float>(i) / 250.0f - 0.1f, i % 2 == 0 ? 1.0f : 0.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LEVEL_FADE_FIELD, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("levelFadeField", exactTolerance, static_cast<float>(levelFadeField(inputs[i][0], inputs[i][1] > 0.5f)), outputs[i][0]);
        }
    }

    // Every field with noise across 0..1 (the keep test by the same noise both sides, so exact).
    void checkKeeps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (std::uint32_t field = 0; field < 128u; ++field) {
            for (int sample = 0; sample < 8; ++sample) {
                inputs.push_back(gpuVec4(static_cast<float>(field), random.between(0.0f, 1.0f)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LEVEL_FADE_KEEPS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const std::uint32_t field = static_cast<std::uint32_t>(inputs[i][0]);
            const float threshold = field == 0u ? 0.0f : levelFadeThreshold(field);
            report.check("levelFadeThreshold", arithmeticTolerance, threshold, outputs[i][0]);

            // A noise within rounding of the threshold may land either side.
            if (std::abs(inputs[i][1] - threshold) > 1e-6f) {
                report.check("levelFadeKeeps", exactTolerance, levelFadeKeeps(field, inputs[i][1]) ? 1.0f : 0.0f, outputs[i][1]);
            }
        }
    }

    /*
    * The noise at pixel centres across a 4K screen and every frame of the cycle: as interleavedGradientNoise, a fraction
    * of a large product (larger here by the frame's turn, up to 5700), so its last digits differ by a few thousandths,
    * measured around the circle (dither_gpu_test.cpp).
    */
    void checkNoise(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 3840.0f)) + 0.5f, std::floor(random.between(0.0f, 2160.0f)) + 0.5f,
                                     std::floor(random.between(0.0f, 1023.0f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LEVEL_FADE_NOISE, inputs, 1, 1);
        const FloatTolerance noiseTolerance{.absolute = 8e-3f, .relative = 0.0f};

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const float expected = levelFadeNoise(inputs[i][0], inputs[i][1], static_cast<std::uint32_t>(inputs[i][2]));
            const float difference = std::abs(outputs[i][0] - expected);
            report.check("levelFadeNoise", noiseTolerance, 0.0f, std::min(difference, 1.0f - difference));
        }
    }

    // Entries with every field, up to the instance limit, with and without the thinned bit.
    void checkEntries(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<std::uint32_t> entries;

        for (std::uint32_t field = 0; field < 128u; ++field) {
            const std::uint32_t entry = ((field * 131071u) & drawListInstanceMask) | (field << drawListFadeShift) | (field % 3 == 0 ? DRAW_LIST_THINNED_BIT : 0u);
            entries.push_back(entry);
            inputs.push_back(gpuVec4(static_cast<float>(entry & 0xffffu), static_cast<float>(entry >> 16u)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DRAW_LIST_FADE, inputs, 1, 1);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            report.check("drawListFade", exactTolerance, static_cast<float>(drawListFade(entries[i])), outputs[i][0]);
        }
    }
}

void gpuTestLevelFade(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("level_fade.glsl");
    checkFields(runner, report);
    checkKeeps(runner, report);
    checkNoise(runner, report);
    checkEntries(runner, report);
}
