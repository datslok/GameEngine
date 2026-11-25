#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/virtual_shadow_rays.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("virtual_shadow_rays_test");

    // Rays of every count and turn: (ray, rays, turn). A radius times a sine or cosine: the trigonometric allowance.
    void checkRayDisc(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int rays : {1, 4, 8, 64}) {
            for (int ray = 0; ray < rays; ++ray) {
                inputs.push_back(gpuVec4(static_cast<float>(ray), static_cast<float>(rays), random.between(0.0f, 1.0f)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_RAY_DISC, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            const Vec2 expected = virtualShadowRayDisc(static_cast<int>(in[0]), static_cast<int>(in[1]), in[2]);
            report.check("virtualShadowRayDisc x", trigonometricTolerance(1.0f), expected.x, outputs[i][0]);
            report.check("virtualShadowRayDisc y", trigonometricTolerance(1.0f), expected.y, outputs[i][1]);
        }
    }

    // Samples up rays in fine and coarse levels, reaching 0 to 6 levels up, some stopped by the depth range's end.
    void checkRaySamples(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{62};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const int samples = 1 + i % 16;
            inputs.push_back(gpuVec4(static_cast<float>(i % samples), static_cast<float>(samples), random.between(0.0f, 0.999f),
                                     i % 7 == 0 ? 0.0f : 0.00465f));
            inputs.push_back(gpuVec4(std::exp(random.between(-1.0f, 8.5f)), std::exp(random.between(-14.0f, -4.0f)), random.between(0.0f, 1.0f),
                                     random.between(-0.7f, 0.7f)));
            inputs.push_back(gpuVec4(random.between(-0.7f, 0.7f), random.between(0.0f, 4096.0f), random.between(0.0f, 4096.0f),
                                     static_cast<float>(i % 7)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_RAY_SAMPLE, inputs, 3, 2);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const GpuVec4& a = inputs[3 * i];
            const GpuVec4& b = inputs[3 * i + 1];
            const GpuVec4& c = inputs[3 * i + 2];
            const int levelsUp = static_cast<int>(c[3]);
            const float share = virtualShadowSampleShare(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2], levelsUp);
            const float height = virtualShadowRayHeight(a[3], b[0], b[1], b[2], levelsUp);
            const Vec3 point = virtualShadowRaySample(Vec3{c[1], c[2], b[2]}, Vec2{b[3], c[0]}, a[3], b[0], b[1], share * height);
            report.check("virtualShadowSampleShare", arithmeticTolerance, share, outputs[2 * i][0]);
            report.check("virtualShadowRayHeight", arithmeticTolerance, height, outputs[2 * i][1]);
            report.check("virtualShadowRaySample", arithmeticTolerance, point, vec3From(outputs[2 * i + 1]));
        }
    }

    // Points across a level and samples round them: (texel x, y, sample texel x, y).
    void checkFootprint(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{63};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float x = random.between(20.0f, 4070.0f);
            const float y = random.between(20.0f, 4070.0f);
            inputs.push_back(gpuVec4(x, y, x + random.between(-12.0f, 12.0f), y + random.between(-12.0f, 12.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_FOOTPRINT, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            const VirtualShadowPage first = virtualShadowFootprintFirst(in[0], in[1]);
            const int index = virtualShadowFootprintIndex(virtualShadowPageOf(in[2], in[3]), first);
            report.check("virtualShadowFootprintFirst", exactTolerance, Vec3{static_cast<float>(first.x), static_cast<float>(first.y), static_cast<float>(index)},
                         vec3From(outputs[i]));
        }
    }

    // Sample levels at and a float step past each level's reach, and stored depths as heights: (offset, levels up,
    // stored, the point's depth), (depth scale).
    void checkSampleLevels(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{65};
        std::vector<GpuVec4> inputs;

        for (int level = 0; level < 8; ++level) {
            const float reach = std::ldexp(virtualShadowRayTexels, level);

            for (float offset : {std::nextafter(reach, 0.0f), reach, std::nextafter(reach, 1e9f)}) {
                inputs.push_back(gpuVec4(offset, static_cast<float>(level % 5), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
                inputs.push_back(gpuVec4(std::exp(random.between(-14.0f, -2.0f))));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_SAMPLE_LEVEL, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[2 * i];
            report.check("virtualShadowSampleLevel", exactTolerance, static_cast<float>(virtualShadowSampleLevel(a[0], static_cast<int>(a[1]))), outputs[i][0]);
            report.check("virtualShadowStoredHeight", arithmeticTolerance, virtualShadowStoredHeight(a[2], a[3], inputs[2 * i + 1][0]), outputs[i][1]);
        }
    }

    // March steps from every state: under or not, a surface sloping or flat, stored above or below the ray, near the
    // surface carried on or far from it, with and without a cap; crossings asked for and read, found or not.
    void checkMarch(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{64};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 600; ++i) {
            const float surface = random.between(0.4f, 0.9f);
            const float rayAt = random.between(0.3f, surface);
            const float ray = rayAt + random.between(0.0f, 0.05f);
            const float stored = i % 3 == 0 ? surface + random.between(-0.002f, 0.002f) : random.between(0.3f, 1.0f);
            inputs.push_back(gpuVec4(static_cast<float>(i % 2), i % 11 == 0 ? 1.0f : 0.0f, surface, rayAt));
            inputs.push_back(gpuVec4(random.between(-0.02f, 0.02f), ray, stored, 0.001f));
            inputs.push_back(gpuVec4(i % 4 == 0 ? 0.0f : 0.0233f, rayAt - random.between(0.0f, 0.01f), random.between(0.3f, 1.0f), i % 5 == 0 ? 0.0f : 1.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_MARCH, inputs, 3, 3);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const GpuVec4& a = inputs[3 * i];
            const GpuVec4& b = inputs[3 * i + 1];
            const GpuVec4& c = inputs[3 * i + 2];
            const VirtualShadowMarch march{.under = a[0] > 0.5f, .hit = a[1] > 0.5f, .surfaceDepth = a[2], .surfaceRayDepth = a[3], .surfaceSlope = b[0],
                                           .lastRayDepth = c[1]};
            const VirtualShadowMarch next = virtualShadowMarchStep(march, b[1], b[2], b[3], c[0]);
            const float crossing = next.checking ? virtualShadowCrossingDepth(next, c[1], b[1]) : 0.0f;
            const std::optional<float> there = c[3] > 0.5f ? std::optional<float>(c[2]) : std::nullopt;
            const VirtualShadowMarch checked = next.checking ? virtualShadowMarchCheck(next, there, b[3]) : next;
            const std::size_t out = 3 * i;
            report.check("virtualShadowMarchStep under", exactTolerance, next.under ? 1.0f : 0.0f, outputs[out][0]);
            report.check("virtualShadowMarchStep hit", exactTolerance, next.hit ? 1.0f : 0.0f, outputs[out][1]);
            report.check("virtualShadowMarchStep surface", arithmeticTolerance, next.surfaceDepth, outputs[out][2]);
            report.check("virtualShadowMarchStep surface ray", arithmeticTolerance, next.surfaceRayDepth, outputs[out][3]);
            report.check("virtualShadowMarchStep slope", arithmeticTolerance, next.surfaceSlope, outputs[out + 1][0]);
            report.check("virtualShadowRayBlocked", exactTolerance, virtualShadowRayBlocked(next) ? 1.0f : 0.0f, outputs[out + 1][1]);
            report.check("virtualShadowMarchStep last ray", arithmeticTolerance, next.lastRayDepth, outputs[out + 1][2]);
            report.check("virtualShadowMarchStep checking", exactTolerance, next.checking ? 1.0f : 0.0f, outputs[out + 1][3]);
            report.check("virtualShadowMarchStep check depth", arithmeticTolerance, next.checking ? next.checkDepth : outputs[out + 2][0], outputs[out + 2][0]);
            report.check("virtualShadowCrossingDepth", arithmeticTolerance, crossing, outputs[out + 2][1]);
            report.check("virtualShadowMarchCheck", exactTolerance, checked.hit ? 1.0f : 0.0f, outputs[out + 2][2]);
        }
    }
}

void gpuTestVirtualShadowRays(GpuFunctionRunner& runner, GpuCheckReport& report) {
    checkRayDisc(runner, report);
    checkRaySamples(runner, report);
    checkFootprint(runner, report);
    checkSampleLevels(runner, report);
    checkMarch(runner, report);
}
