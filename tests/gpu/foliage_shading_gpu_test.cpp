#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/foliage_shading.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("foliage_shading_test");

    // Unit normals and leans of up to the clump's and crown's shares either way, blends 0..1 (the ends included).
    void checkFoliageNormals(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const Vec3 normal = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(0.05f, 1.0f)}.normalized();
            const float blend = i % 10 == 0 ? 0.0f : (i % 10 == 1 ? 1.0f : random.between(0.0f, 1.0f));
            inputs.push_back(gpuVec4(normal.x, normal.y, normal.z, blend));
            inputs.push_back(gpuVec4(random.between(-0.7f, 0.7f), random.between(-0.7f, 0.7f), random.between(-0.7f, 0.7f), 0.0f));
        }

        const std::vector<GpuVec4> normals = runner.run(shader, GPU_TEST_FOLIAGE_NORMAL, inputs, 2, 1);

        // Two normalisations and a dot product: rounding only, a few times the arithmetic allowance.
        for (std::size_t i = 0; i < normals.size(); ++i) {
            const Vec3 normal{inputs[2 * i][0], inputs[2 * i][1], inputs[2 * i][2]};
            const Vec3 lean{inputs[2 * i + 1][0], inputs[2 * i + 1][1], inputs[2 * i + 1][2]};
            const Vec3 expected = blendedFoliageNormal(normal, lean, inputs[2 * i][3]);
            report.check("blendedFoliageNormal", FloatTolerance{.absolute = 1e-5f, .relative = 1e-5f}, expected,
                         Vec3{normals[i][0], normals[i][1], normals[i][2]});
        }
    }

    // Plants, twigs and leaves as small whole numbers (exact as floats); texture coordinates numbered by leaf.
    void checkLeafTones(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{72};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const auto plant = static_cast<float>(i * 7919 % 100000);
            const auto twig = static_cast<float>(i * 31 % 5000);
            const auto leaf = static_cast<float>(i % 18);
            inputs.push_back(gpuVec4(plant, twig, leaf, 0.0f));
            inputs.push_back(gpuVec4(numberedLeafU(random.between(0.01f, 0.99f), static_cast<std::uint32_t>(leaf)), random.between(0.0f, 70.0f), 0.0f, 0.0f));
        }

        const std::vector<GpuVec4> tones = runner.run(shader, GPU_TEST_LEAF_TONE, inputs, 2, 2);

        // The hash and the seed's steps are exact; the scale is a pow.
        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const auto plant = static_cast<std::uint32_t>(inputs[2 * i][0]);
            const auto twig = static_cast<std::uint32_t>(inputs[2 * i][1]);
            const std::uint32_t seed = leafToneSeed(plant, twig, static_cast<std::uint32_t>(inputs[2 * i][2]));
            report.check("leafToneSeed", exactTolerance, static_cast<float>(seed), tones[2 * i][3]);
            report.check("leafToneScale", transcendentalTolerance, leafToneScale(seed), Vec3{tones[2 * i][0], tones[2 * i][1], tones[2 * i][2]});
            report.check("tonedLeafU", arithmeticTolerance, tonedLeafU(inputs[2 * i + 1][0], plant, twig), tones[2 * i + 1][0]);
            report.check("leafToneSeedOf", exactTolerance, static_cast<float>(leafToneSeedOf(inputs[2 * i + 1][1])), tones[2 * i + 1][1]);
        }
    }
}

void gpuTestFoliageShading(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("foliage_shading.glsl");
    checkFoliageNormals(runner, report);
    checkLeafTones(runner, report);
}
