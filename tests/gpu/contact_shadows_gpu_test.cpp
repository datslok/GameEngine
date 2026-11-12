#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/contact_shadows.h"

#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("contact_shadows_test");

    /*
    * Steps across their counts with every noise, steps from far in front to far behind their surfaces after steps in
    * front, inside or behind (round the bias and the reach), pixels all over and off the screen, distances either side of
    * the fades. Inputs per case: (step, steps, noise, length), (previous behind, behind, bias, reach), (pixel, size),
    * (footprint, view distance, fade start, fade end). Outputs: (step distance, bias, blocked, edge fade), (distance
    * fade, step reach).
    */
    void checkContactShadows(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{419};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const int steps = 1 + i % CONTACT_MAX_STEPS;
            const float reach = random.between(0.005f, 0.1f);
            inputs.push_back(GpuVec4{static_cast<float>(i % steps), static_cast<float>(steps), random.between(0.0f, 0.999f), random.between(0.05f, 2.0f)});
            inputs.push_back(GpuVec4{random.between(-0.2f, 0.3f), random.between(-0.2f, 0.3f), random.between(0.0f, 0.05f), reach});
            const float width = random.between(64.0f, 2560.0f);
            const float height = random.between(64.0f, 1440.0f);
            inputs.push_back(GpuVec4{random.between(-20.0f, width + 20.0f), random.between(-20.0f, height + 20.0f), width, height});
            const float start = random.between(0.0f, 60.0f);
            inputs.push_back(GpuVec4{random.between(0.0001f, 0.01f), random.between(0.0f, 120.0f), start, i % 9 == 0 ? start : start + random.between(0.0f, 40.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CONTACT_SHADOWS, inputs, 4, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const GpuVec4* out = &outputs[i * 2];
            report.check("contactStepDistance", arithmeticTolerance, contactStepDistance(static_cast<int>(in[0][0]), static_cast<int>(in[0][1]), in[0][2], in[0][3]),
                         out[0][0]);
            report.check("contactBias", arithmeticTolerance, contactBias(in[3][0], in[3][1]), out[0][1]);
            report.check("contactBlocked", exactTolerance, contactBlocked(in[1][0], in[1][1], in[1][2], in[1][3]) ? 1.0f : 0.0f, out[0][2]);
            report.check("contactEdgeFade", arithmeticTolerance, contactEdgeFade(Vec2{in[2][0], in[2][1]}, Vec2{in[2][2], in[2][3]}), out[0][3]);
            report.check("contactDistanceFade", arithmeticTolerance, contactDistanceFade(in[3][1], in[3][2], in[3][3]), out[1][0]);
            report.check("contactStepReach", arithmeticTolerance, contactStepReach(in[0][3], static_cast<int>(in[0][1])), out[1][1]);
        }
    }


    /*
    * Rays towards lights from points near and far, in and out of range, inside and outside cones (point lights with
    * CONTACT_NO_CONE), nearer than the stop or not. Inputs per case: (position, range), (beam, cosine), (start, stop),
    * (max length, emitter radius). Outputs: (way, length), (stop for that emitter radius).
    */
    void checkContactLightRays(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{421};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 beam = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)}.normalized();
            const float cosine = i % 3 == 0 ? static_cast<float>(CONTACT_NO_CONE) : random.between(0.3f, 0.95f);
            inputs.push_back(GpuVec4{random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f), random.between(0.5f, 6.0f)});
            inputs.push_back(GpuVec4{beam.x, beam.y, beam.z, cosine});
            inputs.push_back(GpuVec4{random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f), random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{random.between(0.05f, 0.6f), random.between(0.0f, 0.2f), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CONTACT_LIGHT_RAYS, inputs, 4, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const GpuVec4* out = &outputs[i * 2];
            const ContactLight light{Vec3{in[0][0], in[0][1], in[0][2]}, in[0][3], Vec3{in[1][0], in[1][1], in[1][2]}, in[1][3], in[2][3]};
            const std::optional<ContactRay> ray = contactLightRay(light, Vec3{in[2][0], in[2][1], in[2][2]}, in[3][0]);
            report.check("contactLightRay length", arithmeticTolerance, ray ? ray->length : 0.0f, out[0][3]);

            if (ray) {
                report.check("contactLightRay way", arithmeticTolerance, ray->toLight.x, out[0][0]);
                report.check("contactLightRay way", arithmeticTolerance, ray->toLight.y, out[0][1]);
                report.check("contactLightRay way", arithmeticTolerance, ray->toLight.z, out[0][2]);
            }

            report.check("contactLightStop", arithmeticTolerance, contactLightStop(in[3][1]), out[1][0]);
        }
    }
}

void gpuTestContactShadows(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("contact_shadows.glsl");
    checkContactShadows(runner, report);
    checkContactLightRays(runner, report);
}