#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/reflection_probe.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("reflection_probe_test");

    // Every face, all over it to its edges, at view distances from the near plane to far away. Inputs per case: (face, s, t, view distance).
    void checkCube(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{113};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            inputs.push_back(GpuVec4{static_cast<float>(i % cubeFaceCount), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f),
                                     random.between(0.05f, 300.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROBE_CUBE, inputs, 1, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            const Vec3 direction = cubeTexelDirection(static_cast<int>(in[0]), Vec2{in[1], in[2]});
            report.check("cubeTexelDirection", arithmeticTolerance, direction, vec3From(outputs[i]));
            report.check("probeRadialDistance", transcendentalTolerance, probeRadialDistance(in[3], direction.normalized()), outputs[i][3]);
        }
    }

    /*
    * The search's step from points near and far from the probe, rays heading out and back past it, stored distances
    * the ray reaches and misses; the confidence from agreement to far apart. Inputs per case: (position, stored
    * distance), (direction, t), (reached, stored, 0, 0).
    */
    void checkParallax(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{127};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 position = random.direction() * random.between(0.0f, 20.0f);
            const float stored = random.between(0.1f, 40.0f);
            inputs.push_back(gpuVec4(position, stored));
            inputs.push_back(gpuVec4(random.direction(), i % 8 == 0 ? -1.0f : random.between(0.0f, 30.0f)));
            inputs.push_back(GpuVec4{stored * random.between(0.7f, 1.3f), stored, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROBE_PARALLAX, inputs, 3, 2);
        const FloatTolerance distanceTolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GpuVec4* out = &outputs[i * 2];
            const Vec3 position = vec3From(in[0]);
            const Vec3 direction = vec3From(in[1]);
            report.check("parallaxDirection", transcendentalTolerance, parallaxDirection(position, direction, in[1][3]), vec3From(out[0]));
            report.check("parallaxStep", distanceTolerance, parallaxStep(position, direction, in[0][3]), out[0][3]);
            report.check("parallaxConfidence", transcendentalTolerance, parallaxConfidence(in[2][0], in[2][1]), out[1][0]);
        }
    }

    /*
    * Levels for every roughness at the presets' probe sizes, source levels over a wide range of densities, and the
    * stand-in blend. Inputs per case: (roughness, levels, pdf, samples), (size, confidence, 0, 0), (stand-in),
    * (specular albedo), (light).
    */
    void checkLevels(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{131};
        std::vector<GpuVec4> inputs;
        const auto colour = [&](float scale) { return Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}; };

        for (int i = 0; i < 256; ++i) {
            const float size = std::exp2(std::floor(random.between(6.0f, 9.0f)));
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), static_cast<float>(probeLevels(static_cast<std::uint32_t>(size))),
                                     std::exp2(random.between(-4.0f, 12.0f)), 64.0f});
            inputs.push_back(GpuVec4{size, random.between(0.0f, 1.0f), 0.0f, 0.0f});
            inputs.push_back(gpuVec4(colour(2.0f)));
            inputs.push_back(gpuVec4(colour(1.0f)));
            inputs.push_back(gpuVec4(colour(50.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROBE_LEVELS, inputs, 5, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const GpuVec4* out = &outputs[i * 2];
            const std::uint32_t levels = static_cast<std::uint32_t>(in[0][1]);
            const std::uint32_t size = static_cast<std::uint32_t>(in[1][0]);
            report.check("probeRoughnessLevel", arithmeticTolerance, probeRoughnessLevel(in[0][0], levels), out[0][0]);
            report.check("prefilterSourceLevel", transcendentalTolerance, prefilterSourceLevel(in[0][2], 64, size), out[0][1]);
            report.check("blendedStandIn", arithmeticTolerance, blendedStandIn(vec3From(in[2]), vec3From(in[3]), vec3From(in[4]), in[1][1]),
                         vec3From(out[1]));
        }
    }
}

void gpuTestReflectionProbe(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("reflection_probe.glsl");
    checkCube(runner, report);
    checkParallax(runner, report);
    checkLevels(runner, report);
}
