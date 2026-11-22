#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/frustum.h"
#include "math/mat4.h"
#include "math/transform.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/twig_records.h"

#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("twig_drawing_test");

    // A random part: placed, turned and scaled evenly (a plant's part), its normal matrix and scale to match.
    GpuInstance randomPart(CaseRandom& random) {
        const float scale = random.between(0.5f, 3.0f);
        const Mat4 model = Transform{.position = Vec3{random.between(-50.0f, 50.0f), random.between(0.0f, 30.0f), random.between(-50.0f, 50.0f)},
                                     .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, random.between(0.0f, 6.28f)),
                                     .scale = Vec3{scale, scale, scale}}
                               .getMatrix();
        GpuInstance part{};
        writeColumnMajor(model, part.model);
        writeColumnMajor(normalMatrix(model), part.normalMatrix);
        part.handedness[0] = 1.0f;
        part.handedness[1] = scale;
        part.material[3] = INSTANCE_KIND_PLAIN;
        part.sway[0] = -1.0f;
        return part;
    }

    // A random twig: a random unit frame (right-handed), scale and foot.
    GpuTwig randomTwig(CaseRandom& random) {
        const Quat turn = Quat::fromAxisAngle(Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(0.1f, 1.0f)}.normalized(),
                                              random.between(0.0f, 6.28f));
        return packTwig(TwigInstance{.foot = Vec3{random.between(-8.0f, 8.0f), random.between(5.0f, 25.0f), random.between(-8.0f, 8.0f)},
                                     .across = turn.rotate(Vec3{1.0f, 0.0f, 0.0f}),
                                     .towardsTip = turn.rotate(Vec3{0.0f, 1.0f, 0.0f}),
                                     .facing = turn.rotate(Vec3{0.0f, 0.0f, 1.0f}),
                                     .scale = random.between(0.5f, 2.0f),
                                     .prototype = 0,
                                     .lean = Vec3{}});
    }

    void pushPart(std::vector<GpuVec4>& inputs, const GpuInstance& part) {
        for (int column = 0; column < 4; ++column) {
            inputs.push_back(gpuVec4(part.model[4 * column], part.model[4 * column + 1], part.model[4 * column + 2], part.model[4 * column + 3]));
        }

        for (int column = 0; column < 4; ++column) {
            inputs.push_back(gpuVec4(part.normalMatrix[4 * column], part.normalMatrix[4 * column + 1], part.normalMatrix[4 * column + 2],
                                     part.normalMatrix[4 * column + 3]));
        }

        inputs.push_back(gpuVec4(part.handedness[1]));
    }

    // A twig's instance: the model and normal matrix within arithmetic rounding of the mirror's, the scale too.
    void checkInstance(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuInstance> parts;
        std::vector<GpuTwig> twigs;
        std::vector<GpuVec4> inputs;

        for (int index = 0; index < 400; ++index) {
            parts.push_back(randomPart(random));
            twigs.push_back(randomTwig(random));
            pushPart(inputs, parts.back());

            for (int row = 0; row < 3; ++row) {
                inputs.push_back(gpuVec4(twigs.back().rows[row][0], twigs.back().rows[row][1], twigs.back().rows[row][2], twigs.back().rows[row][3]));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TWIG_INSTANCE, inputs, 12, 8);

        for (std::size_t i = 0; i < parts.size(); ++i) {
            const GpuInstance expected = twigInstance(parts[i], twigs[i]);

            for (int column = 0; column < 4; ++column) {
                const GpuVec4& got = outputs[8 * i + static_cast<std::size_t>(column)];
                const float* want = &expected.model[4 * column];
                report.check("twigInstance model", arithmeticTolerance, Vec3{want[0], want[1], want[2]}, Vec3{got[0], got[1], got[2]});
            }

            for (int column = 0; column < 3; ++column) {
                const GpuVec4& got = outputs[8 * i + 4 + static_cast<std::size_t>(column)];
                const float* want = &expected.normalMatrix[4 * column];
                report.check("twigInstance normal matrix", arithmeticTolerance, Vec3{want[0], want[1], want[2]}, Vec3{got[0], got[1], got[2]});
            }

            report.check("twigInstance scale", arithmeticTolerance, expected.handedness[1], outputs[8 * i + 7][0]);
        }
    }

    /*
    * A level's choice equals the mirror's, cases where an error's pixels lie within rounding of the pixel error left out
    * (the GPU's square root may round either way there).
    */
    void checkLevelDrawn(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{59};
        struct Case {
            GpuTwigLevel before;
            GpuTwigLevel level;
            bool finest = false;
            GpuInstance twig{};
            GpuCameraCull view{};
        };
        std::vector<Case> all;
        std::vector<GpuVec4> inputs;

        for (int index = 0; index < 1200; ++index) {
            Case c;
            const float radius = random.between(0.1f, 1.0f);
            c.before = GpuTwigLevel{{random.between(-0.3f, 0.3f), random.between(0.0f, 1.0f), 0.0f, radius}, random.between(0.0f, 0.2f), 0, 1, 0};
            c.level = GpuTwigLevel{{c.before.sphere[0], c.before.sphere[1], 0.0f, radius * 1.3f}, index % 9 == 0 ? FLT_MAX : c.before.error + random.between(0.0f, 0.5f), 0, 1, 0};
            c.finest = index % 5 == 0;
            c.twig = twigInstance(randomPart(random), randomTwig(random));
            c.view.levels.eye[0] = random.between(-300.0f, 300.0f);
            c.view.levels.eye[1] = random.between(0.0f, 60.0f);
            c.view.levels.eye[2] = random.between(-300.0f, 300.0f);
            c.view.levels.eye[3] = 1300.0f;
            c.view.levels.pixelError = index % 2 == 0 ? 1.0f : 4.0f;
            c.view.levels.flags = index % 13 == 4 ? CULL_LEVELS_ORTHOGRAPHIC : 0u;
            all.push_back(c);
            const float* m = c.twig.model;
            inputs.push_back(gpuVec4(c.before.sphere[0], c.before.sphere[1], c.before.sphere[2], c.before.sphere[3]));
            inputs.push_back(gpuVec4(c.level.sphere[0], c.level.sphere[1], c.level.sphere[2], c.level.sphere[3]));
            inputs.push_back(gpuVec4(c.before.error, c.level.error, c.finest ? 1.0f : 0.0f, c.view.levels.pixelError));

            for (int column = 0; column < 4; ++column) {
                inputs.push_back(gpuVec4(m[4 * column], m[4 * column + 1], m[4 * column + 2], m[4 * column + 3]));
            }

            inputs.push_back(gpuVec4(c.view.levels.eye[0], c.view.levels.eye[1], c.view.levels.eye[2], c.view.levels.eye[3]));
            inputs.push_back(gpuVec4(static_cast<float>(c.view.levels.flags)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TWIG_LEVEL_DRAWN, inputs, 9, 1);

        for (std::size_t i = 0; i < all.size(); ++i) {
            const Case& c = all[i];
            const float budget = c.view.levels.pixelError;
            const float own = meshClusterErrorPixels(c.before.sphere, c.before.error, c.twig, c.view);
            const float coarser = c.level.error > static_cast<float>(MESH_CLUSTER_NEVER_COARSER) ? 0.0f
                                                                                                  : meshClusterErrorPixels(c.level.sphere, c.level.error, c.twig, c.view);

            if (std::fabs(own - budget) < 1e-4f * budget || std::fabs(coarser - budget) < 1e-4f * budget) {
                continue;
            }

            report.check("twigLevelDrawn", exactTolerance, twigLevelDrawn(c.before, c.finest, c.level, c.twig, c.view) ? 1.0f : 0.0f, outputs[i][0]);
        }
    }

    /*
    * A brick kept equals the mirror's: still parts in the frustum or not, swaying ones near and far from the eye, cases
    * within rounding of a plane or the sway's reach left out.
    */
    void checkBrickKept(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        struct Case {
            GpuInstance part{};
            GpuTwigBrick brick;
            GpuCullRecord record;
            GpuCameraCull view{};
        };
        std::vector<Case> all;
        std::vector<GpuVec4> inputs;

        for (int index = 0; index < 1200; ++index) {
            Case c;
            c.part = randomPart(random);
            c.brick.sphere[0] = random.between(-8.0f, 8.0f);
            c.brick.sphere[1] = random.between(5.0f, 25.0f);
            c.brick.sphere[2] = random.between(-8.0f, 8.0f);
            c.brick.sphere[3] = random.between(0.5f, 4.0f);
            c.record.bounds[0] = c.part.model[12];
            c.record.bounds[1] = c.part.model[13] + 15.0f;
            c.record.bounds[2] = c.part.model[14];
            c.record.bounds[3] = 20.0f;
            c.record.flags = index % 3 == 1 ? CULL_SWAYS : 0u;
            const Vec3 eye{random.between(-80.0f, 80.0f), random.between(1.0f, 40.0f), random.between(-80.0f, 80.0f)};
            const float yaw = random.between(0.0f, 6.28f);
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.9f, 16.0f / 9.0f, 0.1f, 800.0f));
            const Mat4 look = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            LevelView levels;
            levels.eye = eye;
            c.view = packCameraCull(Frustum::fromClipMatrix(projection * look, ClipDepth::OneToZero), levels, false, index % 2 == 0, random.between(10.0f, 80.0f));
            all.push_back(c);
            pushPart(inputs, c.part);
            inputs.push_back(gpuVec4(c.brick.sphere[0], c.brick.sphere[1], c.brick.sphere[2], c.brick.sphere[3]));
            inputs.push_back(gpuVec4(static_cast<float>(c.record.flags), static_cast<float>(c.view.flags), c.view.swayFadeEnd));
            inputs.push_back(gpuVec4(c.record.bounds[0], c.record.bounds[1], c.record.bounds[2], c.record.bounds[3]));
            inputs.push_back(gpuVec4(c.view.levels.eye[0], c.view.levels.eye[1], c.view.levels.eye[2], c.view.levels.eye[3]));

            for (const auto& plane : c.view.frustum.planes) {
                inputs.push_back(gpuVec4(plane[0], plane[1], plane[2], plane[3]));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TWIG_BRICK_KEPT, inputs, 19, 1);

        for (std::size_t i = 0; i < all.size(); ++i) {
            const Case& c = all[i];
            const std::array<float, 4> placed = meshClusterPlacedSphere(c.brick.sphere, c.part);
            bool nearEdge = false;

            for (const auto& plane : c.view.frustum.planes) {
                const float distance = plane[0] * placed[0] + plane[1] * placed[1] + plane[2] * placed[2] + plane[3] + placed[3];
                nearEdge = nearEdge || std::fabs(distance) < 1e-3f;
            }

            const Vec3 offset{c.record.bounds[0] - c.view.levels.eye[0], c.record.bounds[1] - c.view.levels.eye[1], c.record.bounds[2] - c.view.levels.eye[2]};
            nearEdge = nearEdge || std::fabs(offset.length() - c.record.bounds[3] - c.view.swayFadeEnd) < 1e-3f;

            if (nearEdge) {
                continue;
            }

            report.check("twigBrickKept", exactTolerance, twigBrickKept(c.brick, c.part, 0u, c.record, c.view) ? 1.0f : 0.0f, outputs[i][0]);
        }
    }
}

void gpuTestTwigDrawing(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("twig_drawing.glsl");
    checkInstance(runner, report);
    checkLevelDrawn(runner, report);
    checkBrickKept(runner, report);
}
