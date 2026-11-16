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

void gpuTestWind(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("wind.glsl");
    checkNoise(runner, report);
    checkVelocity(runner, report);
    checkSpring(runner, report);
    checkCell(runner, report);
}