#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/quat.h"
#include "math/transform.h"
#include "render/gpu/ground_layers.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("ground_layers_test");

    /*
    * Painted weights from none at all to all four, some layers unpainted (which must never show), heights tied and
    * apart, and blend depths from 0 (the painted weights alone) to past the heights' range (the plain cross-fade).
    * Inputs per case: painted weights, heights, (blend depth, 0, 0, 0).
    */
    void checkBlendWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            GpuVec4 painted{};
            GpuVec4 heights{};

            for (std::size_t layer = 0; layer < 4; ++layer) {
                painted[layer] = (i + static_cast<int>(layer)) % 3 == 0 ? 0.0f : random.between(0.0f, 1.0f);
                heights[layer] = i % 7 == 0 ? 0.5f : random.between(0.0f, 1.0f);
            }

            if (i % 25 == 0) {
                painted = GpuVec4{};
            }

            const float depth = i % 11 == 0 ? 0.0f : random.between(0.0f, 1.5f);
            inputs.push_back(painted);
            inputs.push_back(heights);
            inputs.push_back(GpuVec4{depth, 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GROUND_BLEND_WEIGHTS, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GroundWeights expected = groundBlendWeights(GroundWeights{in[0][0], in[0][1], in[0][2], in[0][3]},
                                                              GroundWeights{in[1][0], in[1][1], in[1][2], in[1][3]}, in[2][0]);

            for (std::size_t layer = 0; layer < 4; ++layer) {
                report.check("groundBlendWeights", arithmeticTolerance, expected[layer], outputs[i][layer]);
            }
        }
    }

    // Places across texels and far out on a big texture, at and between texel centres.
    void checkPaintTaps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{47};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float spread = i % 3 == 0 ? 8.0f : 2048.0f;
            const float x = i % 10 == 0 ? std::floor(random.between(0.0f, spread)) + 0.5f : random.between(0.0f, spread);
            inputs.push_back(GpuVec4{x, random.between(0.0f, spread), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GROUND_PAINT_TAPS, inputs, 1, 2);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GroundPaintTaps expected = groundPaintTaps(Vec2{inputs[i][0], inputs[i][1]});
            const GpuVec4& places = outputs[i * 2];
            const GpuVec4& shares = outputs[i * 2 + 1];
            report.check("groundPaintTaps first", arithmeticTolerance, expected.first.x, places[0]);
            report.check("groundPaintTaps first", arithmeticTolerance, expected.first.y, places[1]);
            report.check("groundPaintTaps second", arithmeticTolerance, expected.second.x, places[2]);
            report.check("groundPaintTaps second", arithmeticTolerance, expected.second.y, places[3]);
            report.check("groundPaintTaps share", arithmeticTolerance, expected.firstShare.x, shares[0]);
            report.check("groundPaintTaps share", arithmeticTolerance, expected.firstShare.y, shares[1]);
        }
    }
}

namespace {
    Vec3 randomDirection(CaseRandom& random) {
        const Vec3 raw{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)};
        return raw.length() > 1e-3f ? raw.normalized() : Vec3{0.0f, 1.0f, 0.0f};
    }

    Vec3 vec3Of(const GpuVec4& value) {
        return Vec3{value[0], value[1], value[2]};
    }

    void checkVec3(GpuCheckReport& report, const char* name, const Vec3& expected, const GpuVec4& output) {
        report.check(name, arithmeticTolerance, expected.x, output[0]);
        report.check(name, arithmeticTolerance, expected.y, output[1]);
        report.check(name, arithmeticTolerance, expected.z, output[2]);
    }

    // Normals all round, squarely along each axis (either side), on the diagonals, and zero.
    void checkProjectionWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            Vec3 normal = randomDirection(random);

            if (i % 20 == 0) {
                const float side = i % 40 == 0 ? 1.0f : -1.0f;
                const int axis = i / 20 % 3;
                normal = Vec3{axis == 0 ? side : 0.0f, axis == 1 ? side : 0.0f, axis == 2 ? side : 0.0f};
            } else if (i % 33 == 0) {
                normal = Vec3{1.0f, 1.0f, i % 2 == 0 ? 0.0f : -1.0f}.normalized();
            } else if (i == 399) {
                normal = Vec3{0.0f, 0.0f, 0.0f};
            }

            inputs.push_back(GpuVec4{normal.x, normal.y, normal.z, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROJECTION_WEIGHTS, inputs, 1, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            checkVec3(report, "projectionWeights", projectionWeights(vec3Of(inputs[i])), outputs[i]);
        }
    }

    // Random normals and tilts (some flat, some steep), with the normal's own weights or arbitrary ones.
    void checkProjectedNormal(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{59};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const Vec3 normal = randomDirection(random);
            inputs.push_back(GpuVec4{normal.x, normal.y, normal.z, 0.0f});

            for (int projection = 0; projection < 3; ++projection) {
                const Vec3 tilt = i % 9 == 0 ? Vec3{0.0f, 0.0f, 1.0f}
                                             : Vec3{random.between(-0.8f, 0.8f), random.between(-0.8f, 0.8f), random.between(0.2f, 1.0f)};
                inputs.push_back(GpuVec4{tilt.x, tilt.y, tilt.z, 0.0f});
            }

            Vec3 weights = projectionWeights(normal);

            if (i % 4 == 0) {
                weights = Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            }

            inputs.push_back(GpuVec4{weights.x, weights.y, weights.z, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROJECTED_NORMAL, inputs, 5, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 5];
            checkVec3(report, "projectedNormal", projectedNormal(vec3Of(in[0]), {vec3Of(in[1]), vec3Of(in[2]), vec3Of(in[3])}, vec3Of(in[4])), outputs[i]);
        }
    }

    // Strata weights with no top layer and each top layer, upward parts below, between and past the limits, and limits
    // out of order.
    void checkStrataPaint(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float start = random.between(0.3f, 0.8f);
            const float full = i % 13 == 0 ? start - 0.1f : start + random.between(0.0f, 0.3f);
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{static_cast<float>(i % 5 - 1), random.between(-1.0f, 1.0f), start, full});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_STRATA_PAINT, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const GroundWeights expected = strataPaint(GroundWeights{in[0][0], in[0][1], in[0][2], in[0][3]}, static_cast<int>(in[1][0]), in[1][1],
                                                       in[1][2], in[1][3]);

            for (std::size_t layer = 0; layer < 4; ++layer) {
                report.check("strataPaint", arithmeticTolerance, expected[layer], outputs[i][layer]);
            }
        }
    }

    // Model matrices turned every way and stretched unevenly, as the pillars and islands are.
    void checkProjectionFrame(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{67};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> models;

        for (int i = 0; i < 200; ++i) {
            const Transform transform{.position = Vec3{random.between(-3000.0f, 3000.0f), random.between(-200.0f, 800.0f), random.between(-3000.0f, 3000.0f)},
                                      .rotation = Quat::fromAxisAngle(randomDirection(random), random.between(-3.0f, 3.0f)),
                                      .scale = Vec3{random.between(0.5f, 150.0f), random.between(0.5f, 350.0f), random.between(0.5f, 150.0f)}};
            const Mat4 model = transform.getMatrix();
            models.push_back(model);

            for (int column = 0; column < 3; ++column) {
                inputs.push_back(GpuVec4{model.values[0][column], model.values[1][column], model.values[2][column], 0.0f});
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PROJECTION_FRAME, inputs, 3, 4);

        for (std::size_t i = 0; i < models.size(); ++i) {
            const ProjectionFrame expected = projectionFrame(models[i]);
            checkVec3(report, "projectionFrame scale", expected.scale, outputs[i * 4]);

            for (std::size_t axis = 0; axis < 3; ++axis) {
                checkVec3(report, "projectionFrame axes", expected.axes[axis], outputs[i * 4 + 1 + axis]);
            }
        }
    }
}

namespace {
    // Heights over the cake, warp texels across their range, points of a pillar on both side projections, stains and
    // strengths from none to full, sides to tops.
    void checkWarpAndStreaks(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            inputs.push_back(GpuVec4{random.between(-400.0f, 1400.0f), random.between(0.0f, 1.0f), random.between(0.0f, 12.0f), 0.0f});
            inputs.push_back(GpuVec4{random.between(-60.0f, 60.0f), random.between(-20.0f, 360.0f), random.between(-60.0f, 60.0f), i % 2 == 0 ? 0.0f : 2.0f});
            inputs.push_back(GpuVec4{1.0f / random.between(2.0f, 20.0f), 1.0f / random.between(50.0f, 400.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_STRATA_WARP_AND_STREAKS, inputs, 4, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const Vec2 place = streakPlace(vec3Of(in[1]), static_cast<int>(in[1][3]), in[2][0], in[2][1]);
            report.check("warpedStrataHeight", arithmeticTolerance, warpedStrataHeight(in[0][0], in[0][1], in[0][2]), outputs[i][0]);
            report.check("streakPlace", arithmeticTolerance, place.x, outputs[i][1]);
            report.check("streakPlace", arithmeticTolerance, place.y, outputs[i][2]);
            report.check("streakShade", arithmeticTolerance, streakShade(in[2][2], in[2][3], in[3][0]), outputs[i][3]);
        }
    }
}

namespace {
    // Places across a few cells and far out (a 300 m pillar at a 4 m tile is 75 tiles up), on cell corners and edges.
    void checkHexTiling(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float spread = i % 3 == 0 ? 3.0f : 120.0f;
            inputs.push_back(GpuVec4{random.between(-spread, spread), random.between(-spread, spread), random.between(0.0f, 1.0f), random.between(0.0f, 0.5f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_HEX_TILING, inputs, 1, 3);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const HexTaps expected = hexTiling(Vec2{inputs[i][0], inputs[i][1]});
            const GpuVec4* out = &outputs[i * 3];
            report.check("hexTiling places", arithmeticTolerance, expected.places[0].x, out[0][0]);
            report.check("hexTiling places", arithmeticTolerance, expected.places[0].y, out[0][1]);
            report.check("hexTiling places", arithmeticTolerance, expected.places[1].x, out[0][2]);
            report.check("hexTiling places", arithmeticTolerance, expected.places[1].y, out[0][3]);
            report.check("hexTiling places", arithmeticTolerance, expected.places[2].x, out[1][0]);
            report.check("hexTiling places", arithmeticTolerance, expected.places[2].y, out[1][1]);
            report.check("variationShade", arithmeticTolerance, variationShade(inputs[i][2], inputs[i][3]), out[1][2]);
            report.check("hexTiling weights", arithmeticTolerance, expected.weights.x, out[2][0]);
            report.check("hexTiling weights", arithmeticTolerance, expected.weights.y, out[2][1]);
            report.check("hexTiling weights", arithmeticTolerance, expected.weights.z, out[2][2]);
        }
    }
}

void gpuTestGroundLayers(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ground_layers.glsl");
    checkBlendWeights(runner, report);
    checkPaintTaps(runner, report);
    checkProjectionWeights(runner, report);
    checkProjectedNormal(runner, report);
    checkStrataPaint(runner, report);
    checkProjectionFrame(runner, report);
    checkWarpAndStreaks(runner, report);
    checkHexTiling(runner, report);
}
