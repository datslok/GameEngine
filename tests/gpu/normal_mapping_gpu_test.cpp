#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/normal_mapping.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("normal_mapping_test");

    // Every texel value from black to white per channel at several scales, including 0 (no normal map) and flipped.
    void checkDecode(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{7};
        std::vector<GpuVec4> inputs;

        for (float scale : {0.0f, 0.5f, 1.0f, 2.0f, -1.0f}) {
            for (int i = 0; i < 40; ++i) {
                inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)}, scale));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DECODE_NORMAL_TEXEL, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("decodeNormalTexel", arithmeticTolerance, decodeNormalTexel(vec3From(inputs[i]), inputs[i][3]), vec3From(outputs[i]));
        }
    }

    /*
    * Interpolated frames as the rasteriser hands them over: normals and tangents a little shorter or longer than 1 and
    * not quite at right angles, both handedness signs, plus a case where everything cancels (the zero result).
    * Inputs per case: (normal, 0), (tangent with its sign in w), (tangent-space normal, 0).
    */
    void checkPerturb(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{11};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const Vec3 normal = random.direction() * random.between(0.8f, 1.2f);
            const Vec3 tangent = random.direction() * random.between(0.8f, 1.2f);
            const float sign = i % 2 == 0 ? 1.0f : -1.0f;
            const Vec3 tilt = decodeNormalTexel(Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.5f, 1.0f)}, 1.0f);
            inputs.push_back(gpuVec4(normal));
            inputs.push_back(gpuVec4(tangent, sign));
            inputs.push_back(gpuVec4(tilt));
        }

        inputs.push_back(gpuVec4(Vec3{0.0f, 0.0f, 1.0f}));
        inputs.push_back(gpuVec4(Vec3{1.0f, 0.0f, 0.0f}, 1.0f));
        inputs.push_back(gpuVec4(Vec3{0.0f, 0.0f, 0.0f}));

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PERTURB_NORMAL, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const Vec4 tangent{in[1][0], in[1][1], in[1][2], in[1][3]};
            report.check("perturbNormal", arithmeticTolerance, perturbNormal(vec3From(in[0]), tangent, vec3From(in[2])), vec3From(outputs[i]));
        }
    }
}

void gpuTestNormalMapping(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("normal_mapping.glsl");
    checkDecode(runner, report);
    checkPerturb(runner, report);
}
