#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/impostor.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("impostor_test");

    // Directions all over the sphere (below the horizon folds onto it), and back.
    void checkFold(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuVec4> directions;
        std::vector<GpuVec4> squares;

        for (int i = 0; i < 500; ++i) {
            const Vec3 direction = Vec3{random.between(-1.0f, 1.0f), random.between(-0.3f, 1.0f), random.between(-1.0f, 1.0f)}.normalized();
            directions.push_back(gpuVec4(direction));
            squares.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> encoded = runner.run(shader, GPU_TEST_IMPOSTOR_ENCODE, directions, 1, 1);
        const std::vector<GpuVec4> decoded = runner.run(shader, GPU_TEST_IMPOSTOR_DECODE, squares, 1, 1);

        for (std::size_t i = 0; i < directions.size(); ++i) {
            const Vec2 expected = hemiOctahedralEncode(Vec3{directions[i][0], directions[i][1], directions[i][2]});
            report.check("hemiOctahedralEncode", arithmeticTolerance, expected.x, encoded[i][0]);
            report.check("hemiOctahedralEncode", arithmeticTolerance, expected.y, encoded[i][1]);
            report.check("hemiOctahedralDecode", transcendentalTolerance, hemiOctahedralDecode(Vec2{squares[i][0], squares[i][1]}),
                         Vec3{decoded[i][0], decoded[i][1], decoded[i][2]});
        }
    }

    // Card points within the sphere read in frames all over an 8 x 8 grid. Inputs: the point, then (column, row, frames).
    void checkFrameUv(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{59};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)}));
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, 8.0f)), std::floor(random.between(0.0f, 8.0f)), 8.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_IMPOSTOR_FRAME_UV, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& point = inputs[2 * i];
            const GpuVec4& frame = inputs[2 * i + 1];
            const Vec2 expected = impostorFrameUv(Vec3{point[0], point[1], point[2]}, impostorFrameDirection(Vec2{frame[0], frame[1]}, 8));
            report.check("impostorFrameUv", transcendentalTolerance, expected.x, outputs[i][0]);
            report.check("impostorFrameUv", transcendentalTolerance, expected.y, outputs[i][1]);
        }
    }
}

void gpuTestImpostor(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("impostor.glsl");
    checkFold(runner, report);
    checkFrameUv(runner, report);
}
