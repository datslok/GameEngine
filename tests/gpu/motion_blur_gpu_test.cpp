#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/motion_blur.h"
#include "render/gpu/taa_shared.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("motion_blur_test");

    // Motions still, slow and past a tile's reach, with and without the camera's own, and samples near and far in depth
    // and speed. Inputs as motion_blur_test.comp lists them.
    void checkMotionBlur(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 600; ++i) {
            const bool noMotion = i % 4 == 0;
            const float scale = i % 3 == 0 ? 0.002f : i % 3 == 1 ? 0.03f : 0.5f;
            const float velocityX = noMotion ? static_cast<float>(TAA_NO_MOTION) : random.between(-scale, scale);
            const float velocityY = noMotion ? static_cast<float>(TAA_NO_MOTION) : random.between(-scale, scale);
            const float pixelDepth = random.between(0.2f, 200.0f);
            const float sampleDepth = i % 5 == 0 ? pixelDepth * random.between(0.99f, 1.01f) : random.between(0.2f, 200.0f);
            inputs.push_back(gpuVec4(velocityX, velocityY, random.between(-scale, scale), random.between(-scale, scale)));
            inputs.push_back(gpuVec4(random.between(640.0f, 3840.0f), random.between(360.0f, 2160.0f), random.between(0.01f, 1.0f), static_cast<float>(i % 2)));
            inputs.push_back(gpuVec4(random.between(0.0f, 40.0f), pixelDepth, sampleDepth, random.between(0.5f, 32.0f)));
            inputs.push_back(gpuVec4(random.between(0.5f, 32.0f), random.between(0.0f, 1.0f), std::floor(random.between(0.0f, static_cast<float>(MOTION_BLUR_SAMPLES) - 0.01f))));
            inputs.push_back(gpuVec4(random.between(-30.0f, 30.0f), random.between(-30.0f, 30.0f), random.between(-30.0f, 30.0f), random.between(-30.0f, 30.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MOTION_BLUR, inputs, 5, 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const GpuVec4* out = &outputs[i * 3];
            const Vec2 velocity{in[0][0], in[0][1]};
            const Vec2 motion = pixelMotion(velocity, Vec2{in[0][2], in[0][3]}, in[1][3] > 0.5f);
            report.check("pixelMotion", arithmeticTolerance, Vec3{motion.x, motion.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});

            // The half-blur of the velocity texel itself (TAA_NO_MOTION's runs past a tile and is cut to it).
            const Vec2 half = motionBlurHalfVector(velocity, Vec2{in[1][0], in[1][1]}, in[1][2]);
            report.check("motionBlurHalfVector", arithmeticTolerance, Vec3{half.x, half.y, 0.0f}, Vec3{out[0][2], out[0][3], 0.0f});

            const Vec2 larger = largerMotion(Vec2{in[4][0], in[4][1]}, Vec2{in[4][2], in[4][3]});
            report.check("largerMotion", exactTolerance, Vec3{larger.x, larger.y, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
            report.check("motionBlurCone", arithmeticTolerance, motionBlurCone(in[2][0], in[2][3]), out[1][2]);
            report.check("motionBlurCylinder", arithmeticTolerance, motionBlurCylinder(in[2][0], in[3][0]), out[1][3]);
            report.check("motionBlurNearer", arithmeticTolerance, motionBlurNearer(in[2][1], in[2][2]), out[2][0]);
            report.check("motionBlurSampleWeight", arithmeticTolerance, motionBlurSampleWeight(in[2][0], in[2][1], in[2][2], in[2][3], in[3][0]), out[2][1]);
            report.check("motionBlurOffset", arithmeticTolerance, motionBlurOffset(static_cast<int>(in[3][2]), in[3][1]), out[2][2]);
        }
    }
}

void gpuTestMotionBlur(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("motion_blur.glsl");
    checkMotionBlur(runner, report);
}
