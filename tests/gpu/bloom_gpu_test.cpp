#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/bloom.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("bloom_test");

    // Colours from black to far brighter than white, where the Karis weight does its work.
    void checkKaris(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{13};
        std::vector<GpuVec4> inputs{gpuVec4(Vec3{0.0f, 0.0f, 0.0f})};

        for (int i = 0; i < 200; ++i) {
            const float scale = i < 100 ? 1.0f : 500.0f;
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_KARIS_WEIGHT, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("karisWeight", arithmeticTolerance, karisWeight(vec3From(inputs[i])), outputs[i][0]);
        }
    }

    // Scene and blur colours with strengths from none to all. Inputs per case: (scene, strength), (bloom, 0).
    void checkMix(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{17};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 200; ++i) {
            const float strength = i % 10 == 0 ? 0.0f : (i % 10 == 1 ? 1.0f : random.between(0.0f, 1.0f));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 10.0f), random.between(0.0f, 10.0f), random.between(0.0f, 10.0f)}, strength));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 10.0f), random.between(0.0f, 10.0f), random.between(0.0f, 10.0f)}));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MIX_BLOOM, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            report.check("mixBloom", arithmeticTolerance, mixBloom(vec3From(in[0]), vec3From(in[1]), in[0][3]), vec3From(outputs[i]));
        }
    }
}

void gpuTestBloom(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("bloom.glsl");
    checkKaris(runner, report);
    checkMix(runner, report);
}
