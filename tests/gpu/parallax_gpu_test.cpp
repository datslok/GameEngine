#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/parallax.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("parallax_test");

    /*
    * Directions taken into interpolated frames: normals and tangents a little off unit length and off square, both
    * handedness signs. Inputs per case: (direction, 0), (normal, 0), (tangent with its sign in w).
    */
    void checkFrame(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{31};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const Vec3 normal = random.direction() * random.between(0.8f, 1.2f);
            Vec3 tangent = random.direction();
            tangent = (tangent - normal.normalized() * normal.normalized().dot(tangent) * random.between(0.9f, 1.0f)) * random.between(0.8f, 1.2f);
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(gpuVec4(normal));
            inputs.push_back(GpuVec4{tangent.x, tangent.y, tangent.z, i % 2 == 0 ? 1.0f : -1.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PARALLAX_FRAME, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const Vec4 tangent{in[2][0], in[2][1], in[2][2], in[2][3]};
            report.check("toTangentFrame", transcendentalTolerance, toTangentFrame(vec3From(in[0]), vec3From(in[1]), tangent), vec3From(outputs[i]));
        }
    }

    /*
    * Steps and shifts for views from straight down to along the surface (and below it, where the cosine is held at the
    * smallest), at several step limits and depths. Inputs per case: (view cosine, most steps, depth, 0), (direction, 0).
    */
    void checkSteps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{37};
        std::vector<GpuVec4> inputs;

        for (int most : {4, 8, 16, 32, 64}) {
            for (int i = 0; i < 40; ++i) {
                const Vec3 direction = random.direction();
                inputs.push_back(gpuVec4(direction.z, static_cast<float>(most), random.between(0.0f, 0.1f)));
                inputs.push_back(gpuVec4(direction));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PARALLAX_STEPS, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const GpuVec4* out = &outputs[i * 2];
            const Vec3 direction = vec3From(in[1]);
            const Vec2 delta = parallaxDelta(direction, in[0][2]);
            const Vec2 lightDelta = parallaxLightDelta(direction, in[0][2]);
            report.check("parallaxSteps", exactTolerance, static_cast<float>(parallaxSteps(in[0][0], static_cast<int>(in[0][1]))), out[0][0]);
            report.check("parallaxDelta", arithmeticTolerance, delta.x, out[0][1]);
            report.check("parallaxDelta", arithmeticTolerance, delta.y, out[0][2]);
            report.check("parallaxLightDelta", arithmeticTolerance, lightDelta.x, out[1][0]);
            report.check("parallaxLightDelta", arithmeticTolerance, lightDelta.y, out[1][1]);
        }
    }

    /*
    * The gap, the crossing (including a step that ends exactly on the ground), the depth in pixels and its fade over the
    * whole range, and shadow samples below, at and above the ray. Inputs per case: (t, height, gap above, gap below),
    * (depth, step across, step down's x), (step down's y, terrain, ray height, climbed).
    */
    void checkScalars(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float gapBelow = i % 10 == 0 ? 0.0f : -random.between(0.0f, 1.0f);
            const float footprint = random.between(0.0f, 0.05f);
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.001f, 1.0f), gapBelow});
            inputs.push_back(GpuVec4{random.between(0.0f, 0.05f), footprint, random.between(-0.02f, 0.02f), random.between(-0.02f, 0.02f)});
            inputs.push_back(GpuVec4{random.between(-0.02f, 0.02f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.01f, 1.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PARALLAX_SCALARS, inputs, 3, 2);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GpuVec4* out = &outputs[i * 2];
            const float pixels = parallaxDepthPixels(in[1][0], Vec2{in[1][1], in[1][2]}, Vec2{in[1][3], in[2][0]});
            report.check("parallaxGap", arithmeticTolerance, parallaxGap(in[0][0], in[0][1]), out[0][0]);
            report.check("parallaxCrossing", arithmeticTolerance, parallaxCrossing(in[0][2], in[0][3]), out[0][1]);
            report.check("parallaxDepthPixels", transcendentalTolerance, pixels, out[0][2]);
            report.check("parallaxFade", transcendentalTolerance, parallaxFade(pixels), out[0][3]);
            report.check("parallaxShadowSample", arithmeticTolerance, parallaxShadowSample(in[2][1], in[2][2], in[2][3]), out[1][0]);
        }
    }
}

void gpuTestParallax(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("parallax.glsl");
    checkFrame(runner, report);
    checkSteps(runner, report);
    checkScalars(runner, report);
}
