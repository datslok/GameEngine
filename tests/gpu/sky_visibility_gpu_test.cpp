#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/sky_visibility.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("sky_visibility_test");

    Vec3 randomUnit(CaseRandom& random) {
        const Vec3 direction{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)};
        const float length = direction.length();
        return length > 1e-3f ? direction * (1.0f / length) : Vec3{0.0f, 1.0f, 0.0f};
    }

    /*
    * Bands across their range, some overshooting either way (so the clamp is met), unit normals and directions, bounce
    * shares and blockers' floors from 0 to 1. Inputs per case: (a, b), (normal, bounce share), (direction, floor).
    * Outputs: (facing, along, in air, blockers lit), (the light with the fill: the flat ambient (1, 0.5, 0.25), the sky
    * seen facing the normal, the bounce guess, a fill of the direction's absolute values).
    */
    void checkShares(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{307};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            inputs.push_back(gpuVec4(random.between(-0.2f, 1.2f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(randomUnit(random), i % 4 == 0 ? 0.0f : random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(randomUnit(random), i % 3 == 0 ? 0.0f : random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SKY_VISIBILITY_SHARES, inputs, 3, 2);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const GpuVec4& bands = inputs[3 * i];
            const SkyVisibility visibility{bands[0], Vec3{bands[1], bands[2], bands[3]}};
            const Vec3 normal = vec3From(inputs[3 * i + 1]);
            const float bounce = inputs[3 * i + 1][3];
            const Vec3 direction = vec3From(inputs[3 * i + 2]);
            const float floor = inputs[3 * i + 2][3];
            const GpuVec4& shares = outputs[2 * i];
            report.check("skyVisibilityFacing", arithmeticTolerance, skyVisibilityFacing(visibility, normal, bounce, floor), shares[0]);
            report.check("skyVisibilityAlong", arithmeticTolerance, skyVisibilityAlong(visibility, direction, bounce, floor), shares[1]);
            report.check("skyVisibilityInAir", arithmeticTolerance, skyVisibilityInAir(visibility, bounce, floor), shares[2]);
            report.check("skyBlockersLit", arithmeticTolerance, skyBlockersLit(visibility, floor), shares[3]);

            const Vec3 fill{std::abs(direction.x), std::abs(direction.y), std::abs(direction.z)};
            const Vec3 light = skyLightWithFill(Vec3{1.0f, 0.5f, 0.25f}, skySeenFacing(visibility, normal), bounce * skyBlockersLit(visibility, floor), fill);
            report.check("skyLightWithFill", arithmeticTolerance, light, vec3From(outputs[2 * i + 1]));
        }
    }

    // A word's bits carried in a float (the test buffers hold floats; the shader takes the bits back unchanged).
    float wordAsFloat(std::uint32_t word) {
        return std::bit_cast<float>(word);
    }

    /*
    * Points packed by packSkyCell, read back by the shader. Inputs per case: the four words. Outputs: (a, b), (reaches
    * along +x +y +z, state), (reaches along -x -y -z). Halves and bytes come back exactly.
    */
    void checkUnpack(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{311};
        std::vector<GpuVec4> inputs;
        std::vector<SkyCell> cells;

        for (int i = 0; i < 256; ++i) {
            SkyCell cell;
            cell.visibility = SkyVisibility{random.between(0.0f, 1.0f), Vec3{random.between(-1.5f, 1.5f), random.between(-1.5f, 1.5f), random.between(-1.5f, 1.5f)}};
            for (float& reach : cell.reach) {
                reach = i % 5 == 0 ? 1.0f : random.between(0.0f, 1.0f);
            }
            cell.state = static_cast<SkyCellState>(i % 3);
            const GpuSkyCell packed = packSkyCell(cell);
            cells.push_back(unpackSkyCell(packed));
            inputs.push_back(GpuVec4{wordAsFloat(packed.words[0]), wordAsFloat(packed.words[1]), wordAsFloat(packed.words[2]), wordAsFloat(packed.words[3])});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SKY_VISIBILITY_UNPACK, inputs, 1, 3);

        for (std::size_t i = 0; i < cells.size(); ++i) {
            const SkyCell& cell = cells[i];
            report.check("unpackSkyCell bands", exactTolerance, cell.visibility.open, outputs[3 * i][0]);
            report.check("unpackSkyCell bands", exactTolerance, cell.visibility.lean, Vec3{outputs[3 * i][1], outputs[3 * i][2], outputs[3 * i][3]});
            report.check("unpackSkyCell reaches", arithmeticTolerance, Vec3{cell.reach[0], cell.reach[2], cell.reach[4]}, vec3From(outputs[3 * i + 1]));
            report.check("unpackSkyCell reaches", arithmeticTolerance, Vec3{cell.reach[1], cell.reach[3], cell.reach[5]}, vec3From(outputs[3 * i + 2]));
            report.check("unpackSkyCell state", exactTolerance, static_cast<float>(cell.state), outputs[3 * i + 1][3]);
        }
    }

    /*
    * Reaches across 0..1 and offsets within a spacing either way, some exactly at a reach. Inputs per case: (reaches
    * along +x +y +z, spacing), (reaches along -x -y -z), (offset from the point). Output: (sees).
    */
    void checkCornerSees(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{313};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float spacing = random.between(0.25f, 4.0f);
            const Vec3 positive{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            const Vec3 negative{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            Vec3 offset = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)} * spacing;

            if (i % 8 == 0) {
                offset.x = -negative.x * spacing * 0.5f;
            }

            inputs.push_back(gpuVec4(positive, spacing));
            inputs.push_back(gpuVec4(negative));
            inputs.push_back(gpuVec4(offset));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SKY_CORNER_SEES, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec3 positive = vec3From(inputs[3 * i]);
            const Vec3 negative = vec3From(inputs[3 * i + 1]);
            const std::array<float, skyReachDirections> reach{positive.x, negative.x, positive.y, negative.y, positive.z, negative.z};
            const bool sees = skyCornerSees(reach, vec3From(inputs[3 * i + 2]), inputs[3 * i][3]);
            report.check("skyCornerSees", exactTolerance, sees ? 1.0f : 0.0f, outputs[i][0]);
        }
    }
}

void gpuTestSkyVisibility(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("sky_visibility.glsl");
    checkShares(runner, report);
    checkUnpack(runner, report);
    checkCornerSees(runner, report);
}
