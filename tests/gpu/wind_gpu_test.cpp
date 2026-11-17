#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/angles.h"
#include "render/gpu/wind.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("wind_test");

    // Places across lattice cells, negatives included: the same hashes, smoothstep blends within rounding.
    void checkNoise(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-300.0f, 300.0f), random.between(-300.0f, 300.0f), random.between(-50.0f, 50.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_NOISE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("windNoise", arithmeticTolerance, windNoise(Vec3{inputs[i][0], inputs[i][1], inputs[i][2]}), outputs[i][0]);
        }
    }

    /*
    * Winds of every direction and speed at places across a valley and times into a session. Inputs: the direction
    * (x, z) and speed; the gusts' strength and size and the turbulence's; the place and time. The noise's lattice
    * coordinates are large, so a coordinate's rounding moves the result by its slope times an ulp.
    */
    void checkVelocity(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{67};
        std::vector<GpuVec4> inputs;
        std::vector<Wind> winds;

        for (int i = 0; i < 500; ++i) {
            Wind wind;
            wind.towardsDegrees = random.between(0.0f, 360.0f);
            wind.speed = random.between(0.0f, 15.0f);
            wind.gustStrength = random.between(0.0f, 1.0f);
            wind.gustSize = random.between(5.0f, 200.0f);
            wind.turbulence = random.between(0.0f, 1.0f);
            wind.turbulenceSize = random.between(1.0f, 50.0f);
            const float angle = radiansFromDegrees(wind.towardsDegrees);
            inputs.push_back(gpuVec4(std::cos(angle), std::sin(angle), wind.speed));
            inputs.push_back(gpuVec4(wind.gustStrength, wind.gustSize, wind.turbulence, wind.turbulenceSize));
            inputs.push_back(gpuVec4(random.between(-1000.0f, 1000.0f), random.between(-1000.0f, 1000.0f), random.between(0.0f, 600.0f)));
            winds.push_back(wind);
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_VELOCITY, inputs, 3, 1);
        const FloatTolerance tolerance{.absolute = 2e-3f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& place = inputs[3 * i + 2];
            const Vec2 expected = windVelocity(winds[i], Vec2{place[0], place[1]}, place[2]);
            report.check("windVelocity", tolerance, expected.x, outputs[i][0]);
            report.check("windVelocity", tolerance, expected.y, outputs[i][1]);
        }
    }

    // Springs of every category's range stepped from any state by frame steps up to past the longest. Inputs: the state;
    // the target, frequency and damping; the step.
    void checkSpring(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f)));
            inputs.push_back(gpuVec4(random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(0.05f, 5.0f), random.between(0.0f, 2.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 0.2f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_SPRING, inputs, 3, 1);
        const FloatTolerance tolerance{.absolute = 1e-5f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& state = inputs[3 * i];
            const GpuVec4& spring = inputs[3 * i + 1];
            const Vec4 expected = windSpringStep(Vec4{state[0], state[1], state[2], state[3]}, Vec2{spring[0], spring[1]}, spring[2], spring[3],
                                                 inputs[3 * i + 2][0]);
            report.check("windSpringStep", tolerance, expected.x, outputs[i][0]);
            report.check("windSpringStep", tolerance, expected.y, outputs[i][1]);
            report.check("windSpringStep", tolerance, expected.z, outputs[i][2]);
            report.check("windSpringStep", tolerance, expected.w, outputs[i][3]);
        }
    }

    // Texels and windows anywhere, negatives included: exact. Inputs: the texel (x, z) and the window's first cell.
    void checkCell(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 64.0f)), std::floor(random.between(0.0f, 64.0f)),
                                     std::floor(random.between(-5000.0f, 5000.0f)), std::floor(random.between(-5000.0f, 5000.0f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_CELL, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const WindCell expected = windCellOfTexel(static_cast<int>(inputs[i][0]), static_cast<int>(inputs[i][1]),
                                                      WindWindow{static_cast<int>(inputs[i][2]), static_cast<int>(inputs[i][3])});
            report.check("windCellOfTexel", exactTolerance, static_cast<float>(expected.x), outputs[i][0]);
            report.check("windCellOfTexel", exactTolerance, static_cast<float>(expected.z), outputs[i][1]);
        }
    }
}

namespace {
    // Leans from a breeze's to a storm's, either way along both axes.
    void checkSoftLean(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{79};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_SOFT_LEAN, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Vec2 expected = windSoftLean(Vec2{inputs[i][0], inputs[i][1]});
            report.check("windSoftLean", arithmeticTolerance, expected.x, outputs[i][0]);
            report.check("windSoftLean", arithmeticTolerance, expected.y, outputs[i][1]);
        }
    }

    // Places in, near the edges of and outside windows anywhere. Inputs: the place and the window's first cell; the cell.
    void checkWindowShare(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{83};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float cell = random.between(0.5f, 20.0f);
            const float originX = std::floor(random.between(-500.0f, 500.0f));
            const float originZ = std::floor(random.between(-500.0f, 500.0f));
            const float across = cell * static_cast<float>(WIND_CELLS);
            inputs.push_back(gpuVec4(originX * cell + random.between(-0.2f, 1.2f) * across, originZ * cell + random.between(-0.2f, 1.2f) * across, originX,
                                     originZ));
            inputs.push_back(gpuVec4(cell));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_WINDOW_SHARE, inputs, 2, 1);
        const FloatTolerance tolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& place = inputs[2 * i];
            const float expected = windWindowShare(Vec2{place[0], place[1]}, WindWindow{static_cast<int>(place[2]), static_cast<int>(place[3])},
                                                   inputs[2 * i + 1][0]);
            report.check("windWindowShare", tolerance, expected, outputs[i][0]);
        }
    }

    /*
    * Vertices of trees, plants and grass of every size, in any wind, faded or not, at times into a session. Inputs: the
    * vertex from its foot and its height in the mesh; its place and the model's scale; the sway; the spring, fade and
    * time; the flutter's rate (hertz, metres). The flutter's and branches' sines of large phases are the GPU's own, so the tolerance is a few millimetres.
    */
    void checkSway(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{89};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float height = random.between(0.2f, 50.0f);
            const float scale = random.between(0.5f, 2.0f);
            const float modelHeight = random.between(0.0f, 1.2f) * height;
            inputs.push_back(gpuVec4(random.between(-8.0f, 8.0f) * scale, modelHeight * scale, random.between(-8.0f, 8.0f) * scale, modelHeight));
            inputs.push_back(gpuVec4(random.between(-2000.0f, 2000.0f), random.between(-50.0f, 300.0f), random.between(-2000.0f, 2000.0f), scale));
            inputs.push_back(gpuVec4(std::floor(random.between(-1.0f, 4.0f)), height, random.between(0.0f, 1.0f), random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.between(-0.3f, 0.3f), random.between(-0.3f, 0.3f), random.between(0.0f, 1.0f), random.between(0.0f, 600.0f)));
            inputs.push_back(gpuVec4(random.between(0.5f, 4.0f), random.between(0.3f, 12.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_SWAY, inputs, 5, 1);
        const FloatTolerance tolerance{.absolute = 3e-3f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& vertex = inputs[5 * i];
            const GpuVec4& place = inputs[5 * i + 1];
            const GpuVec4& sway = inputs[5 * i + 2];
            const GpuVec4& spring = inputs[5 * i + 3];
            const GpuVec4& rate = inputs[5 * i + 4];
            const Vec3 expected = windSwayOffset(Vec3{vertex[0], vertex[1], vertex[2]}, vertex[3], Vec3{place[0], place[1], place[2]},
                                                 Vec4{sway[0], sway[1], sway[2], sway[3]}, place[3], Vec2{spring[0], spring[1]}, spring[2],
                                                 Vec2{rate[0], rate[1]}, spring[3]);
            report.check("windSwayOffset", tolerance, expected.x, outputs[i][0]);
            report.check("windSwayOffset", tolerance, expected.y, outputs[i][1]);
            report.check("windSwayOffset", tolerance, expected.z, outputs[i][2]);
        }
    }
}

namespace {
    /*
    * Anchors on trees of every size, in any wind, faded or not. Inputs: the record's foot, point and bend; the spring,
    * fade and time. Its sines' large phases are the GPU's own, as windSwayOffset's.
    */
    void checkAnchor(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{97};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float height = random.between(5.0f, 50.0f);
            const float scale = random.between(0.5f, 2.0f);
            const Vec3 foot{random.between(-2000.0f, 2000.0f), random.between(-50.0f, 100.0f), random.between(-2000.0f, 2000.0f)};
            inputs.push_back(gpuVec4(foot.x, foot.y, foot.z, height));
            inputs.push_back(gpuVec4(foot.x + random.between(-10.0f, 10.0f), foot.y + random.between(0.2f, 1.0f) * height * scale,
                                     foot.z + random.between(-10.0f, 10.0f), scale));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(-0.3f, 0.3f), random.between(-0.3f, 0.3f), random.between(0.0f, 1.0f), random.between(0.0f, 600.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_ANCHOR, inputs, 4, 1);
        const FloatTolerance tolerance{.absolute = 3e-3f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            GpuWindAnchor anchor{};

            for (std::size_t component = 0; component < 4; ++component) {
                anchor.foot[component] = inputs[4 * i][component];
                anchor.point[component] = inputs[4 * i + 1][component];
                anchor.bend[component] = inputs[4 * i + 2][component];
            }

            const GpuVec4& spring = inputs[4 * i + 3];
            const Vec3 expected = windAnchorOffset(anchor, Vec2{spring[0], spring[1]}, spring[2], spring[3]);
            report.check("windAnchorOffset", tolerance, expected.x, outputs[i][0]);
            report.check("windAnchorOffset", tolerance, expected.y, outputs[i][1]);
            report.check("windAnchorOffset", tolerance, expected.z, outputs[i][2]);
        }
    }

    // Arms from none to a long root's, swings and flutters of every size. Inputs: the place and arm; the swing and
    // flutter; the spring, fade and time; the flutter's rate.
    void checkSwing(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{101};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-2000.0f, 2000.0f), random.between(-50.0f, 600.0f), random.between(-2000.0f, 2000.0f),
                                     random.between(0.0f, 100.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 2.0f), random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.between(-0.3f, 0.3f), random.between(-0.3f, 0.3f), random.between(0.0f, 1.0f), random.between(0.0f, 600.0f)));
            inputs.push_back(gpuVec4(random.between(0.5f, 4.0f), random.between(0.3f, 12.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_SWING, inputs, 4, 1);
        const FloatTolerance tolerance{.absolute = 3e-3f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& place = inputs[4 * i];
            const GpuVec4& swing = inputs[4 * i + 1];
            const GpuVec4& spring = inputs[4 * i + 2];
            const GpuVec4& rate = inputs[4 * i + 3];
            const Vec3 expected = windSwingOffset(place[3], Vec3{place[0], place[1], place[2]}, swing[0], swing[1], Vec2{spring[0], spring[1]}, spring[2],
                                                  Vec2{rate[0], rate[1]}, spring[3]);
            report.check("windSwingOffset", tolerance, expected.x, outputs[i][0]);
            report.check("windSwingOffset", tolerance, expected.y, outputs[i][1]);
            report.check("windSwingOffset", tolerance, expected.z, outputs[i][2]);
        }
    }

    // Anchors' motions blended at any share, plus a swing. Inputs: the first motion and the share; the second; the swing.
    void checkHanging(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{103};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f), random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f)));
            inputs.push_back(gpuVec4(random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_HANGING, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& first = inputs[3 * i];
            const GpuVec4& second = inputs[3 * i + 1];
            const GpuVec4& swing = inputs[3 * i + 2];
            const Vec3 expected = windHangingOffset(Vec3{first[0], first[1], first[2]}, Vec3{second[0], second[1], second[2]}, first[3],
                                                    Vec3{swing[0], swing[1], swing[2]});
            report.check("windHangingOffset", arithmeticTolerance, expected.x, outputs[i][0]);
            report.check("windHangingOffset", arithmeticTolerance, expected.y, outputs[i][1]);
            report.check("windHangingOffset", arithmeticTolerance, expected.z, outputs[i][2]);
        }
    }
}

namespace {
    /*
    * Leaves' shiver at every rate the settings allow in practice, at places across the world and times into a session.
    * Inputs: the place and amount; the rate (hertz, metres) and time. Its sines' large phases are the GPU's own.
    */
    void checkFlutter(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{107};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(-2000.0f, 2000.0f), random.between(-50.0f, 600.0f), random.between(-2000.0f, 2000.0f),
                                     random.between(0.0f, 0.3f)));
            inputs.push_back(gpuVec4(random.between(0.5f, 4.0f), random.between(0.3f, 12.0f), random.between(0.0f, 600.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_FLUTTER, inputs, 2, 1);
        const FloatTolerance tolerance{.absolute = 3e-3f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& place = inputs[2 * i];
            const GpuVec4& rate = inputs[2 * i + 1];
            const Vec3 expected = windFlutterOffset(Vec3{place[0], place[1], place[2]}, place[3], Vec2{rate[0], rate[1]}, rate[2]);
            report.check("windFlutterOffset", tolerance, expected.x, outputs[i][0]);
            report.check("windFlutterOffset", tolerance, expected.y, outputs[i][1]);
            report.check("windFlutterOffset", tolerance, expected.z, outputs[i][2]);
        }
    }
}

namespace {
    /*
    * Strands from none to a 200 m vine's, against bands of every falling order. Inputs: the swing's length and the bands'
    * frequencies. Outputs: the shares and the chain's frequency; logs and square roots within rounding.
    */
    void checkChain(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{109};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float shortBand = random.between(0.3f, 2.0f);
            const float mediumBand = shortBand * random.between(0.1f, 0.9f);
            inputs.push_back(gpuVec4(random.between(0.0f, 200.0f), shortBand, mediumBand, mediumBand * random.between(0.1f, 0.9f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_CHAIN, inputs, 1, 1);
        const FloatTolerance tolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& input = inputs[i];
            const Vec3 shares = windChainShares(input[0], Vec3{input[1], input[2], input[3]});
            report.check("windChainShares", tolerance, shares.x, outputs[i][0]);
            report.check("windChainShares", tolerance, shares.y, outputs[i][1]);
            report.check("windChainShares", tolerance, shares.z, outputs[i][2]);
            report.check("windChainHertz", tolerance, windChainHertz(input[0]), outputs[i][3]);
        }
    }
}

void gpuTestWind(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("wind.glsl");
    checkNoise(runner, report);
    checkVelocity(runner, report);
    checkSpring(runner, report);
    checkCell(runner, report);
    checkSoftLean(runner, report);
    checkWindowShare(runner, report);
    checkSway(runner, report);
    checkAnchor(runner, report);
    checkSwing(runner, report);
    checkFlutter(runner, report);
    checkChain(runner, report);
    checkHanging(runner, report);
}