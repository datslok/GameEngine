#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/transparency.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("transparency_test");

    Vec3 randomColour(CaseRandom& random, float most) {
        return Vec3{random.between(0.0f, most), random.between(0.0f, most), random.between(0.0f, most)};
    }

    /*
    * Coverage, transmission and metallic across 0..1 (each end included), light from dark to bright. Inputs per case:
    * (alpha, transmission, metallic), diffuse light, specular light, emissive, base colour, specular albedo. Outputs:
    * the light added, the transmittance.
    */
    void checkBlend(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{307};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float alpha = i % 7 == 0 ? 1.0f : (i % 11 == 0 ? 0.0f : random.between(0.0f, 1.0f));
            const float transmission = i % 5 == 0 ? 0.0f : (i % 3 == 0 ? 1.0f : random.between(0.0f, 1.0f));
            inputs.push_back(GpuVec4{alpha, transmission, i % 9 == 0 ? 1.0f : random.between(0.0f, 1.0f), 0.0f});
            inputs.push_back(gpuVec4(randomColour(random, 10.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 10.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 2.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TRANSPARENT_BLEND, inputs, 6, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 6];
            const TransparentLight expected = transparentBlend(in[0][0], in[0][1], in[0][2], vec3From(in[1]), vec3From(in[2]), vec3From(in[3]),
                                                               vec3From(in[4]), vec3From(in[5]));
            report.check("transparentBlend", arithmeticTolerance, expected.added, vec3From(outputs[i * 2]));
            report.check("transparentBlend", arithmeticTolerance, expected.transmittance, vec3From(outputs[i * 2 + 1]));
        }
    }

    /*
    * The reactive mask's share: pictures from black to very bright, the glass's picture darker, brighter or the same,
    * scales up to 3 and limits across 0..1. Inputs per case: the picture before, the picture after, (scale, limit).
    * Output: x the share.
    */
    void checkReactiveShare(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{311};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 before = i % 13 == 0 ? Vec3{0.0f, 0.0f, 0.0f} : randomColour(random, i % 2 == 0 ? 1.0f : 1000.0f);
            const Vec3 after = i % 5 == 0 ? before : before * random.between(0.0f, 3.0f) + randomColour(random, 0.1f);
            inputs.push_back(gpuVec4(before, 0.0f));
            inputs.push_back(gpuVec4(after, 0.0f));
            inputs.push_back(GpuVec4{random.between(0.0f, 3.0f), i % 7 == 0 ? 1.0f : random.between(0.0f, 1.0f), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_REACTIVE_SHARE, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            report.check("reactiveShare", arithmeticTolerance, reactiveShare(vec3From(in[0]), vec3From(in[1]), in[2][0], in[2][1]), outputs[i][0]);
        }
    }
}

void gpuTestTransparency(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("transparency.glsl");
    checkBlend(runner, report);
    checkReactiveShare(runner, report);
}
