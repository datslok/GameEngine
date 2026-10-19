#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/light_falloff.h"
#include "render/gpu/shadow_map.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("lighting_test");

    // Distances from the light itself to past the range, for small and large ranges and sources.
    void checkFalloff(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (float range : {1.0f, 8.0f, 60.0f}) {
            for (float sourceRadius : {0.1f, 1.0f, 3.0f}) {
                for (int i = 0; i <= 30; ++i) {
                    inputs.push_back(gpuVec4(range * static_cast<float>(i) / 25.0f, range, sourceRadius));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_DISTANCE_FALLOFF, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("distanceFalloff", arithmeticTolerance, distanceFalloff(inputs[i][0], inputs[i][1], inputs[i][2]), outputs[i][0]);
        }
    }

    // Cosines across a narrow and a wide cone, outside, between and inside.
    void checkSpotCone(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (const GpuVec4& cone : {gpuVec4(0.9f, 0.97f), gpuVec4(0.5f, 0.8f)}) {
            for (int i = 0; i <= 50; ++i) {
                inputs.push_back(gpuVec4(0.4f + 0.6f * static_cast<float>(i) / 50.0f, cone[0], cone[1]));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SPOT_CONE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("spotCone", arithmeticTolerance, spotCone(inputs[i][0], inputs[i][1], inputs[i][2]), outputs[i][0]);
        }
    }

    /*
    * The cube face for random directions and for the ties along the cube's edges and corners, where the comparisons'
    * order decides: a different face there would read the wrong shadow tile, so it must match exactly.
    */
    void checkShadowFace(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{3};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(random.direction() * random.between(0.1f, 50.0f)));
        }

        for (float x : {-1.0f, 0.0f, 1.0f}) {
            for (float y : {-1.0f, 0.0f, 1.0f}) {
                for (float z : {-1.0f, 0.0f, 1.0f}) {
                    inputs.push_back(gpuVec4(Vec3{x, y, z}));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_POINT_SHADOW_FACE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("pointShadowFace", exactTolerance, static_cast<float>(pointShadowFace(vec3From(inputs[i]))), outputs[i][0]);
        }
    }
}

void gpuTestLighting(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("lighting.glsl");
    checkFalloff(runner, report);
    checkSpotCone(runner, report);
    checkShadowFace(runner, report);
}
