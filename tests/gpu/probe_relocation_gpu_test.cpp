#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/probe_relocation.h"

#include <string>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("probe_relocation_test");

    // Must match STEP_CASE_RAYS in probe_relocation_test.comp.
    constexpr int stepCaseRays = 8;

    // Offsets inside and past the limit, steps along every way, uneven limits.
    void checkEscapeScale(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{91};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const Vec3 limit{random.between(0.05f, 1.0f), random.between(0.05f, 1.0f), random.between(0.05f, 1.0f)};
            inputs.push_back(gpuVec4(Vec3{random.between(-1.0f, 1.0f) * limit.x, random.between(-1.0f, 1.0f) * limit.y,
                                          random.between(-1.0f, 1.0f) * limit.z}));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.01f, 2.0f)));
            inputs.push_back(gpuVec4(limit));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ESCAPE_SCALE, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const float expected = probeEscapeScale(vec3From(inputs[i * 3]), vec3From(inputs[i * 3 + 1]), vec3From(inputs[i * 3 + 2]));
            report.check("probeEscapeScale", arithmeticTolerance, expected, outputs[i][0]);
        }
    }

    /*
    * A tally of a few rays (back faces, front faces near and far, misses) and the step and record that follow, with the
    * probe somewhere in its cell: the rays are few, so each case's share of back faces lands on both sides of the
    * inside threshold, and near front faces on both sides of the nudge's.
    */
    void checkRelocationStep(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{92};
        std::vector<GpuVec4> inputs;
        const int inputsPerCase = 2 + stepCaseRays;

        for (int i = 0; i < 400; ++i) {
            const Vec3 spacing{random.between(0.2f, 2.0f), random.between(0.2f, 2.0f), random.between(0.2f, 2.0f)};
            inputs.push_back(gpuVec4(Vec3{random.between(-0.3f, 0.3f) * spacing.x, random.between(-0.3f, 0.3f) * spacing.y,
                                          random.between(-0.3f, 0.3f) * spacing.z}));
            inputs.push_back(gpuVec4(spacing));

            for (int ray = 0; ray < stepCaseRays; ++ray) {
                const float kind = random.between(0.0f, 1.0f);
                const float distance = kind < 0.15f ? probeRayMiss
                                       : kind < 0.45f ? -random.between(0.01f, 2.0f)
                                       : kind < 0.6f  ? random.between(0.001f, 0.1f)
                                                      : random.between(0.1f, 4.0f);
                inputs.push_back(gpuVec4(random.direction(), distance));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_RELOCATION_STEP, inputs, inputsPerCase, 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * static_cast<std::size_t>(inputsPerCase)];
            const Vec3 offset = vec3From(in[0]);
            const Vec3 spacing = vec3From(in[1]);
            ProbeRayTally tally;

            for (int ray = 0; ray < stepCaseRays; ++ray) {
                tallyProbeRay(tally, vec3From(in[2 + ray]), in[2 + ray][3], offset, spacing);
            }

            const ProbeMove move = relocateProbeStep(tally, offset, spacing);
            const GpuProbeRelocation relocation = finishProbeRelocation(tally, offset, move.stuck);
            const GpuVec4* out = &outputs[i * 3];

            report.check("tallyProbeRay faces", exactTolerance, Vec3{static_cast<float>(tally.backFaces), static_cast<float>(tally.frontFaces), 0.0f},
                         Vec3{out[0][0], out[0][1], 0.0f});
            report.check("tallyProbeRay distances", exactTolerance, Vec3{tally.closestBackFace, tally.closestFrontFace, tally.farthestFrontFace},
                         Vec3{out[0][2], out[0][3], out[1][0]});
            report.check("tallyProbeRay surface beyond cells", arithmeticTolerance, tally.surfaceBeyondCells, out[1][1]);
            report.check("relocateProbeStep stuck", exactTolerance, move.stuck ? 1.0f : 0.0f, out[1][2]);
            report.check("finishProbeRelocation off", exactTolerance, relocation.off, out[1][3]);
            report.check("relocateProbeStep offset", arithmeticTolerance, move.offset, vec3From(out[2]));
        }
    }
}

void gpuTestProbeRelocation(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("probe_relocation.glsl");
    checkEscapeScale(runner, report);
    checkRelocationStep(runner, report);
}
