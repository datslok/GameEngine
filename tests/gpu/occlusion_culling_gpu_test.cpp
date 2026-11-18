#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/occlusion_culling.h"

#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("occlusion_culling_test");

    /*
    * Spheres all over the view, near and far, small and large, some reaching the near plane, through the GPU's
    * rectangle, nearest depth and level against the mirror's. The rectangle's corners are floored pixel positions: a
    * square root and a division apart from the CPU's, a corner can land a pixel the other side of a pixel edge, hence
    * a pixel's tolerance there (the margin round each rectangle already allows for a pixel either way).
    */
    void checkSphere(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const RenderSize size{1920, 1080};
        const std::uint32_t levels = depthPyramidLevels(size);
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(std::numbers::pi_v<float> / 3.0f, 16.0f / 9.0f, 0.1f, 5000.0f));
        const OcclusionView view = makeOcclusionView(Mat4::identity(), projection, 0.1f, size);
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 600; ++i) {
            const float distance = random.between(0.05f, 2000.0f);
            const Vec3 centre{random.between(-0.8f, 0.8f) * distance, random.between(-0.5f, 0.5f) * distance, -distance};
            const float radius = random.between(0.01f, 0.4f) * distance;
            inputs.push_back(gpuVec4(centre, radius));
            inputs.push_back(gpuVec4(view.scaleX, view.scaleY, view.nearPlane));
            inputs.push_back(gpuVec4(static_cast<float>(size.width), static_cast<float>(size.height), static_cast<float>(levels)));
            inputs.push_back(gpuVec4(view.depthConstant, view.depthPerInverseDistance));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_OCCLUSION_SPHERE, inputs, 4, 2);
        const FloatTolerance onePixel{.absolute = 1.0f, .relative = 0.0f};

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const Vec3 centre{in[0][0], in[0][1], in[0][2]};
            const std::optional<PixelRect> rect = sphereRectFromView(centre, in[0][3], view.scaleX, view.scaleY, view.nearPlane, size);
            const GpuVec4& gpuRect = outputs[i * 2];
            const GpuVec4& gpuRest = outputs[i * 2 + 1];

            report.check("occlusionSphereRect projected", exactTolerance, rect ? 1.0f : 0.0f, gpuRest[0]);

            if (!rect) {
                continue;
            }

            report.check("occlusionSphereRect", onePixel, static_cast<float>(rect->minX), gpuRect[0]);
            report.check("occlusionSphereRect", onePixel, static_cast<float>(rect->minY), gpuRect[1]);
            report.check("occlusionSphereRect", onePixel, static_cast<float>(rect->maxX), gpuRect[2]);
            report.check("occlusionSphereRect", onePixel, static_cast<float>(rect->maxY), gpuRect[3]);
            report.check("occlusionNearestDepth", arithmeticTolerance, nearestDepthFromView(centre.z, in[0][3], view.depthConstant, view.depthPerInverseDistance),
                         gpuRest[1]);

            // The level from the GPU's own rectangle, so a pixel's difference above cannot change it here.
            const PixelRect fromGpu{static_cast<int>(gpuRect[0]), static_cast<int>(gpuRect[1]), static_cast<int>(gpuRect[2]), static_cast<int>(gpuRect[3])};
            report.check("occlusionLevel", exactTolerance, static_cast<float>(occlusionLevel(fromGpu, levels)), gpuRest[2]);
        }
    }
}

void gpuTestOcclusionCulling(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("occlusion_culling.glsl");
    checkSphere(runner, report);
}
