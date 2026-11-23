#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/liana_trees.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("liana_trees_test");

    // Feet on cells' edges, a float either side of them and inside, several sizes and below 0: whole numbers, exact.
    void checkCells(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (const float size : {128.0f, 64.0f, 37.3f, 0.7f}) {
            for (int step = -400; step <= 400; ++step) {
                const float edge = static_cast<float>(step / 4) * size;
                const int kind = ((step % 4) + 4) % 4;
                const float x = kind == 0 ? edge : kind == 1 ? std::nextafter(edge, -1e9f) : kind == 2 ? std::nextafter(edge, 1e9f) : edge + 0.37f * size;
                inputs.push_back(gpuVec4(x, -x * 0.5f, size));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LIANA_TREE_CELL, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const PlantingCell cell = lianaTreeCell(inputs[i][0], inputs[i][1], inputs[i][2]);
            report.check("lianaTreeCell", exactTolerance, static_cast<float>(cell.column), outputs[i][0]);
            report.check("lianaTreeCell", exactTolerance, static_cast<float>(cell.row), outputs[i][1]);
        }
    }

    // Cells either side of 0 in tables of several sides.
    void checkPlaces(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (const int side : {1, 10, 13, 19}) {
            for (int column = -45; column <= 45; column += 7) {
                for (int row = -40; row <= 40; row += 9) {
                    inputs.push_back(gpuVec4(static_cast<float>(column), static_cast<float>(row), static_cast<float>(side)));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LIANA_CELL_PLACE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const PlantingCell cell{static_cast<int>(inputs[i][0]), static_cast<int>(inputs[i][1])};
            report.check("lianaCellPlace", exactTolerance, static_cast<float>(lianaCellPlace(cell, static_cast<std::uint32_t>(inputs[i][2]))), outputs[i][0]);
        }
    }

    // Feet the same, a little apart and further, either side of the match's distance (none on it: rounding decides there).
    void checkSameTree(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{31};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const Vec3 foot{random.between(-3000.0f, 3000.0f), random.between(-50.0f, 400.0f), random.between(-3000.0f, 3000.0f)};
            const float apart = i % 2 == 0 ? random.between(0.0f, 0.008f) : random.between(0.012f, 0.5f);
            inputs.push_back(gpuVec4(foot));
            inputs.push_back(gpuVec4(foot + Vec3{apart, 0.0f, 0.0f}));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LIANA_SAME_TREE, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec3 planted{inputs[2 * i][0], inputs[2 * i][1], inputs[2 * i][2]};
            const Vec3 foot{inputs[2 * i + 1][0], inputs[2 * i + 1][1], inputs[2 * i + 1][2]};
            report.check("lianaSameTree", exactTolerance, lianaSameTree(planted, foot) ? 1.0f : 0.0f, outputs[i][0]);
        }
    }
}

void gpuTestLianaTrees(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("liana_trees.glsl");
    checkCells(runner, report);
    checkPlaces(runner, report);
    checkSameTree(runner, report);
}
