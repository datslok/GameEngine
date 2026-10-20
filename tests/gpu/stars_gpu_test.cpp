#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/stars.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("stars_test");

    /*
    * Cameras looking all round with views from narrow to wide, stars in front of them, the magnitudes from Sirius to the
    * faintest, offsets a pixel and more either side. Inputs per case: (right, magnitude), (up, brightness), (forward,
    * contrast), (direction, cosine off the axis), (offset, width, height).
    */
    void checkStars(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = 5;

        for (int i = 0; i < 400; ++i) {
            const Vec3 forward = random.direction();
            const Vec3 side = forward.cross(std::abs(forward.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f}).normalized();
            const Vec3 right = side * random.between(0.2f, 2.0f);
            const Vec3 up = right.cross(forward).normalized() * random.between(0.2f, 1.5f);
            const Vec3 direction = cameraRayDirection(right, up, forward, random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)).normalized();
            inputs.push_back(gpuVec4(right, random.between(-1.5f, 6.5f)));
            inputs.push_back(gpuVec4(up, random.between(1e-7f, 1e-5f)));
            inputs.push_back(gpuVec4(forward, random.between(0.3f, 1.2f)));
            inputs.push_back(gpuVec4(direction, direction.dot(forward)));
            inputs.push_back(GpuVec4{random.between(-1.5f, 1.5f), random.between(-1.5f, 1.5f), random.between(320.0f, 3840.0f),
                                     random.between(240.0f, 2160.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_STARS, inputs, static_cast<std::uint32_t>(perCase), 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * perCase];
            const GpuVec4* out = &outputs[i * 2];
            const Vec2 position = starScreenPosition(vec3From(in[0]), vec3From(in[1]), vec3From(in[2]), vec3From(in[3]));

            report.check("starScreenPosition", arithmeticTolerance, Vec3{position.x, position.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});
            report.check("starLight", transcendentalTolerance, starLight(in[0][3], in[1][3], in[2][3]), out[0][2]);
            report.check("starSplatWeight", arithmeticTolerance, starSplatWeight(Vec2{in[4][0], in[4][1]}), out[0][3]);
            report.check("pixelSolidAngle", arithmeticTolerance, pixelSolidAngle(vec3From(in[0]), vec3From(in[1]), in[4][2], in[4][3], in[3][3]),
                         out[1][0]);
        }
    }
}

void gpuTestStars(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("stars.glsl");
    checkStars(runner, report);
}
