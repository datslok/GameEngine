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

    // Bands across their range, some overshooting either way (so the clamp is met).
    GpuVec4 randomBands(CaseRandom& random, float most) {
        return gpuVec4(random.between(-0.2f * most, 1.2f * most), random.between(-most, most), random.between(-most, most), random.between(-most, most));
    }

    SkyVisibility bandsFrom(const GpuVec4& bands) {
        return SkyVisibility{bands[0], Vec3{bands[1], bands[2], bands[3]}};
    }

    /*
    * The sky's bands and three bounce channels' (smaller, as a bounce is) across their range, unit normals and
    * directions, floors from 0 to 0.2. Inputs per case: (a, b), (normal, floor), (direction, unused), the red, green and
    * blue bounce bands. Outputs: facing, along and in the air, each per channel.
    */
    void checkShares(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{307};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            inputs.push_back(randomBands(random, 1.0f));
            inputs.push_back(gpuVec4(randomUnit(random), i % 4 == 0 ? 0.0f : random.between(0.0f, 0.2f)));
            inputs.push_back(gpuVec4(randomUnit(random), 0.0f));

            for (int channel = 0; channel < 3; ++channel) {
                inputs.push_back(i % 5 == 0 ? gpuVec4(0.0f, 0.0f, 0.0f, 0.0f) : randomBands(random, 0.6f));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SKY_VISIBILITY_SHARES, inputs, 6, 3);

        for (std::size_t i = 0; i < inputs.size() / 6; ++i) {
            const GpuVec4* in = &inputs[6 * i];
            const SkyVisibility visibility = bandsFrom(in[0]);
            const Vec3 normal = vec3From(in[1]);
            const float floor = in[1][3];
            const Vec3 direction = vec3From(in[2]);
            const SkyBounce bounce{bandsFrom(in[3]), bandsFrom(in[4]), bandsFrom(in[5])};
            const GpuVec4* out = &outputs[3 * i];
            report.check("skyLightFacing", arithmeticTolerance, skyLightFacing(visibility, bounce, normal, floor), vec3From(out[0]));
            report.check("skyLightAlong", arithmeticTolerance, skyLightAlong(visibility, bounce, direction, floor), vec3From(out[1]));
            report.check("skyLightInAir", arithmeticTolerance, skyLightInAir(visibility, bounce, floor), vec3From(out[2]));
        }
    }

    // A word's bits carried in a float (the test buffers hold floats; the shader takes the bits back unchanged).
    float wordAsFloat(std::uint32_t word) {
        return std::bit_cast<float>(word);
    }

    /*
    * Points packed by packSkyCell, read back by the shader. Inputs per case: the twelve words. Outputs: (a, b), (reaches
    * along +x +y +z, state), (reaches along -x -y -z), the red, green and blue bounce bands. Halves and bytes come back
    * exactly.
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

            for (SkyVisibility& channel : cell.bounce) {
                channel = bandsFrom(randomBands(random, 0.8f));
            }

            const GpuSkyCell packed = packSkyCell(cell);
            cells.push_back(unpackSkyCell(packed));

            for (std::size_t texel = 0; texel < 3; ++texel) {
                inputs.push_back(GpuVec4{wordAsFloat(packed.words[4 * texel]), wordAsFloat(packed.words[4 * texel + 1]), wordAsFloat(packed.words[4 * texel + 2]),
                                         wordAsFloat(packed.words[4 * texel + 3])});
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SKY_VISIBILITY_UNPACK, inputs, 3, 6);

        for (std::size_t i = 0; i < cells.size(); ++i) {
            const SkyCell& cell = cells[i];
            const GpuVec4* out = &outputs[6 * i];
            report.check("unpackSkyCell bands", exactTolerance, cell.visibility.open, out[0][0]);
            report.check("unpackSkyCell bands", exactTolerance, cell.visibility.lean, Vec3{out[0][1], out[0][2], out[0][3]});
            report.check("unpackSkyCell reaches", arithmeticTolerance, Vec3{cell.reach[0], cell.reach[2], cell.reach[4]}, vec3From(out[1]));
            report.check("unpackSkyCell reaches", arithmeticTolerance, Vec3{cell.reach[1], cell.reach[3], cell.reach[5]}, vec3From(out[2]));
            report.check("unpackSkyCell state", exactTolerance, static_cast<float>(cell.state), out[1][3]);

            for (std::size_t channel = 0; channel < 3; ++channel) {
                const GpuVec4& bands = out[3 + channel];
                report.check("unpackSkyCell bounce", exactTolerance, cell.bounce[channel].open, bands[0]);
                report.check("unpackSkyCell bounce", exactTolerance, cell.bounce[channel].lean, Vec3{bands[1], bands[2], bands[3]});
            }
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
