#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/camera_finish.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {
    const std::string shader = gpuTestShader("camera_finish_test");

    // Light from black to bright and coloured, through grades from flat to strong. Inputs: (light, gain), (contrast,
    // saturation, lift, gamma).
    void checkGrade(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{31};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float scale = i % 3 == 0 ? 0.05f : i % 3 == 1 ? 1.0f : 8.0f;
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}, random.between(0.5f, 2.0f)));
            inputs.push_back(gpuVec4(random.between(0.5f, 2.0f), random.between(0.0f, 2.0f), random.between(-0.05f, 0.05f), random.between(0.5f, 2.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CAMERA_GRADE, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            CameraSettings settings;
            settings.gain = in[0][3];
            settings.contrast = in[1][0];
            settings.saturation = in[1][1];
            settings.lift = in[1][2];
            settings.gamma = in[1][3];
            report.check("gradeColour", transcendentalTolerance, gradeColour(vec3From(in[0]), settings), vec3From(outputs[i]));
        }
    }

    // Places across a screen, fields of view, shifts, strengths and noise. Inputs as camera_finish_test.comp lists them.
    void checkLens(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{37};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float width = random.between(640.0f, 3840.0f);
            const float height = random.between(360.0f, 2160.0f);
            const float peak = i % 2 == 0 ? 1.0f : random.between(1.0f, 6.0f);
            const float level = random.between(0.0f, peak);
            inputs.push_back(gpuVec4(random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(0.2f, 2.0f), random.between(0.2f, 1.5f)));
            inputs.push_back(gpuVec4(random.between(0.0f, width), random.between(0.0f, height), width, height));
            inputs.push_back(gpuVec4(random.between(-8.0f, 8.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), peak));
            inputs.push_back(gpuVec4(Vec3{level, level * random.between(0.5f, 1.0f), level * random.between(0.0f, 1.0f)}));
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 3840.0f)), std::floor(random.between(0.0f, 2160.0f)), std::floor(random.between(0.0f, 65535.0f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CAMERA_LENS, inputs, 5, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const GpuVec4* out = &outputs[i * 2];
            report.check("naturalVignetting", arithmeticTolerance, naturalVignetting(Vec2{in[0][0], in[0][1]}, Vec2{in[0][2], in[0][3]}, in[2][1]), out[0][0]);
            const Vec2 place = aberrationPlace(Vec2{in[1][0], in[1][1]}, Vec2{in[1][2], in[1][3]}, in[2][0]);
            report.check("aberrationPlace", arithmeticTolerance, Vec3{place.x, place.y, 0.0f}, Vec3{out[0][1], out[0][2], 0.0f});
            report.check("grainNoise", exactTolerance,
                         grainNoise(static_cast<std::uint32_t>(in[4][0]), static_cast<std::uint32_t>(in[4][1]), static_cast<std::uint32_t>(in[4][2])), out[0][3]);
            report.check("filmGrain", arithmeticTolerance, filmGrain(vec3From(in[3]), in[2][2], in[2][1], in[2][3]), vec3From(out[1]));
        }
    }
}

void gpuTestCameraFinish(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("camera_finish.glsl");
    checkGrade(runner, report);
    checkLens(runner, report);
}
