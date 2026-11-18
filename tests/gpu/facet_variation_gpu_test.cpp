#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/facet_variation.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("facet_variation_test");

    // Facets' numbers across 0..1, amounts from none to more than the tone can take, roughnesses across 0..1.
    std::vector<GpuVec4> facetInputs() {
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), i % 10 == 0 ? 0.0f : random.between(0.0f, 1.5f), random.between(0.0f, 1.0f)));
        }

        return inputs;
    }

    // Both are a multiply and an add, so they match to rounding.
    void checkFacets(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<GpuVec4> inputs = facetInputs();
        const std::vector<GpuVec4> tones = runner.run(shader, GPU_TEST_FACET_TONE, inputs, 1, 1);
        const std::vector<GpuVec4> roughnesses = runner.run(shader, GPU_TEST_FACET_ROUGHNESS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Vec2 numbers{inputs[i][0], inputs[i][1]};
            report.check("facetToneScale", arithmeticTolerance, facetToneScale(numbers, inputs[i][2]), tones[i][0]);
            report.check("facetRoughness", arithmeticTolerance, facetRoughness(inputs[i][3], numbers, inputs[i][2]), roughnesses[i][0]);
        }
    }
}

void gpuTestFacetVariation(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("facet_variation.glsl");
    checkFacets(runner, report);
}
