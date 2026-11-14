#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/cascades.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("cascades_test");

    // Points inside, on and outside boxes of different depths: (x, y, depth, depth over radius).
    void checkEdgeDistance(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{21};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(random.between(-1.2f, 1.2f), random.between(-1.2f, 1.2f), random.between(-0.1f, 1.1f),
                                     random.between(2.0f, 40.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CASCADE_EDGE_DISTANCE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("cascadeEdgeDistance", arithmeticTolerance, cascadeEdgeDistance(Vec3{in[0], in[1], in[2]}, in[3]), outputs[i][0]);
        }
    }

    // Surfaces from facing the sun to edge-on and beyond: (base offset, cosine of the angle to the sun).
    void checkSunBias(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{23};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(random.between(0.001f, 2.0f), random.between(-0.2f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SUN_SHADOW_BIAS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("sunShadowBias", arithmeticTolerance, sunShadowBias(inputs[i][0], inputs[i][1]), outputs[i][0]);
        }
    }

    // Points kilometres from the camera moved up to 2 km towards suns all round, and pairs of shadows (glass tints):
    // (point, reach), (towards the sun), (the cascade's shadow), (the distant blockers').
    void checkDistantOccluders(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{24};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const Vec3 toLight = Vec3{random.between(-1.0f, 1.0f), random.between(0.05f, 1.0f), random.between(-1.0f, 1.0f)}.normalized();
            inputs.push_back(gpuVec4(random.between(-3000.0f, 3000.0f), random.between(-50.0f, 800.0f), random.between(-3000.0f, 3000.0f),
                                     random.between(0.0f, 2000.0f)));
            inputs.push_back(gpuVec4(toLight.x, toLight.y, toLight.z));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DISTANT_OCCLUDERS, inputs, 4, 2);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const GpuVec4& point = inputs[4 * i];
            const GpuVec4& toLight = inputs[4 * i + 1];
            const GpuVec4& cascade = inputs[4 * i + 2];
            const GpuVec4& distant = inputs[4 * i + 3];
            const Vec3 moved = distantOccluderPoint(Vec3{point[0], point[1], point[2]}, Vec3{toLight[0], toLight[1], toLight[2]}, point[3]);
            const Vec3 darker = withDistantOccluders(Vec3{cascade[0], cascade[1], cascade[2]}, Vec3{distant[0], distant[1], distant[2]});

            for (int axis = 0; axis < 3; ++axis) {
                report.check("distantOccluderPoint", arithmeticTolerance, moved[axis], outputs[2 * i][static_cast<std::size_t>(axis)]);
                report.check("withDistantOccluders", exactTolerance, darker[axis], outputs[2 * i + 1][static_cast<std::size_t>(axis)]);
            }
        }
    }

    // Edges either side of the band, bands down to none: (edge distance, band, weight, noise).
    void checkBlendAndDither(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{22};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float band = i % 10 == 0 ? 0.0f : random.between(0.01f, 0.3f);
            inputs.push_back(gpuVec4(random.between(-0.2f, 0.5f), band, random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CASCADE_BLEND, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            report.check("cascadeBlendWeight", arithmeticTolerance, cascadeBlendWeight(in[0], in[1]), outputs[i][0]);
            report.check("cascadeDitherKeeps", exactTolerance, cascadeDitherKeeps(in[2], in[3]) ? 1.0f : 0.0f, outputs[i][1]);
        }
    }
}

void gpuTestCascades(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("cascades.glsl");
    checkEdgeDistance(runner, report);
    checkBlendAndDither(runner, report);
    checkSunBias(runner, report);
    checkDistantOccluders(runner, report);
}
