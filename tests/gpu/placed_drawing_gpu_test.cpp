#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "render/gpu/column_major.h"
#include "render/gpu/placed_drawing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

/*
* placed_drawing.glsl against render/gpu/placed_drawing.cpp: the plant's matrix, a sphere placed, and a part's instance
* placed. The GPU's sine and cosine round differently from the CPU's (the only steps not exact), so the matrix's turned
* parts agree to their rounding, scaled by the plant: a few millionths of a metre on a 40 m tree.
*/
namespace {
    const std::string shader = gpuTestShader("placed_drawing_test");

    // The sine and cosine's rounding, carried through a scale of up to 1.2 and a sphere of up to 30 m.
    constexpr FloatTolerance turnedTolerance{.absolute = 2e-5f, .relative = 1e-5f};

    struct PlacedCase {
        GpuPlacedPlant plant{};
        float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    std::vector<PlacedCase> cases() {
        CaseRandom random{53};
        std::vector<PlacedCase> made;

        for (int i = 0; i < 2000; ++i) {
            PlacedCase each;
            each.plant.foot[0] = random.between(-3000.0f, 3000.0f);
            each.plant.foot[1] = random.between(-50.0f, 300.0f);
            each.plant.foot[2] = random.between(-3000.0f, 3000.0f);
            each.plant.scale = random.between(0.8f, 1.2f);
            each.plant.turnRadians = random.between(-6.3f, 12.6f);
            each.sphere[0] = random.between(-5.0f, 5.0f);
            each.sphere[1] = random.between(0.0f, 30.0f);
            each.sphere[2] = random.between(-5.0f, 5.0f);
            each.sphere[3] = i % 10 == 0 ? -1.0f : random.between(0.5f, 30.0f);
            made.push_back(each);
        }

        return made;
    }

    std::vector<GpuVec4> inputsOf(const std::vector<PlacedCase>& made) {
        std::vector<GpuVec4> inputs;

        for (const PlacedCase& each : made) {
            inputs.push_back(gpuVec4(each.plant.foot[0], each.plant.foot[1], each.plant.foot[2], each.plant.scale));
            inputs.push_back(gpuVec4(each.plant.turnRadians));
            inputs.push_back(gpuVec4(each.sphere[0], each.sphere[1], each.sphere[2], each.sphere[3]));
        }

        return inputs;
    }

    void checkModel(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<PlacedCase> made = cases();
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLACED_PLANT_MODEL, inputsOf(made), 3, 4);

        for (std::size_t i = 0; i < made.size(); ++i) {
            const Mat4 expected = placedPlantModel(made[i].plant);

            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    report.check("placedPlantModel", turnedTolerance, expected.values[row][column], outputs[4 * i + static_cast<std::size_t>(column)][row]);
                }
            }
        }
    }

    void checkSphere(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<PlacedCase> made = cases();
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLACED_SPHERE, inputsOf(made), 3, 1);

        for (std::size_t i = 0; i < made.size(); ++i) {
            const std::array<float, 4> expected = placedSphere(made[i].sphere, made[i].plant);

            for (int axis = 0; axis < 4; ++axis) {
                report.check("placedSphere", turnedTolerance, expected[static_cast<std::size_t>(axis)], outputs[i][axis]);
            }
        }
    }

    // The part as the shader builds it: the identity in the plant's frame, still, its level sphere the case's.
    void checkInstance(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<PlacedCase> made = cases();
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLACED_INSTANCE, inputsOf(made), 3, 4);

        for (std::size_t i = 0; i < made.size(); ++i) {
            GpuInstance part{};
            writeColumnMajor(Mat4::identity(), part.model);
            writeColumnMajor(Mat4::identity(), part.normalMatrix);
            part.handedness[0] = 1.0f;
            part.handedness[1] = 1.0f;
            part.sway[0] = -1.0f;
            std::copy(std::begin(made[i].sphere), std::end(made[i].sphere), part.levelSphere);
            const GpuInstance expected = placedInstance(part, made[i].plant);

            for (int row = 0; row < 4; ++row) {
                report.check("placedInstance normals", turnedTolerance, expected.normalMatrix[row], outputs[4 * i][row]);
                report.check("placedInstance normals", turnedTolerance, expected.normalMatrix[8 + row], outputs[4 * i + 1][row]);
                report.check("placedInstance level sphere", turnedTolerance, expected.levelSphere[row], outputs[4 * i + 2][row]);
            }

            report.check("placedInstance scale", exactTolerance, expected.handedness[1], outputs[4 * i + 3][1]);
        }
    }
}

void gpuTestPlacedDrawing(GpuFunctionRunner& runner, GpuCheckReport& report) {
    checkModel(runner, report);
    checkSphere(runner, report);
    checkInstance(runner, report);
}
