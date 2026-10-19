#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/overlay_composite.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("overlay_test");

    /*
    * Premultiplied texels as ImGui writes them (colour at most alpha, every 8-bit step of alpha including none) for an
    * SDR white of 1 and HDR white levels. Inputs per case: (texel), (white level, 0, 0, 0).
    */
    void checkOverlayOutput(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{29};
        std::vector<GpuVec4> inputs;

        for (float whiteLevel : {1.0f, 2.5f, 3.75f}) {
            for (int alphaStep = 0; alphaStep <= 255; alphaStep += 5) {
                const float alpha = static_cast<float>(alphaStep) / 255.0f;
                const Vec3 colour{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
                inputs.push_back(gpuVec4(colour * alpha, alpha));
                inputs.push_back(gpuVec4(whiteLevel));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_OVERLAY_OUTPUT, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const Vec4 expected = overlayOutput(Vec4{in[0][0], in[0][1], in[0][2], in[0][3]}, in[1][0]);
            report.check("overlayOutput colour", transcendentalTolerance, Vec3{expected.x, expected.y, expected.z}, vec3From(outputs[i]));
            report.check("overlayOutput alpha", exactTolerance, expected.w, outputs[i][3]);
        }
    }
}

void gpuTestOverlay(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("overlay.glsl");
    checkOverlayOutput(runner, report);
}
