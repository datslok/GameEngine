#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_depth_range.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("cull_records_test");

    struct Case {
        GpuCullRecord record;
        GpuMeshLevels levels;
        GpuLevelView view;
        GpuFrustum frustum;
    };

    /*
    * Whether any of the case's level tests lands within a hair of its threshold, where the GPU's square root may round
    * the other way: a choice either side is right there, so those cases are left out of the comparison.
    */
    bool nearAThreshold(const Case& c) {
        const Vec3 eye{c.view.eye[0], c.view.eye[1], c.view.eye[2]};
        const bool orthographic = (c.view.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u;
        const float distance = levelSphereDistance(c.record.levelSphere, eye);
        const auto near = [&](float errorMetres) {
            const float ratio = levelErrorInPixels(errorMetres, distance, c.view.eye[3], orthographic) / c.view.pixelError;
            return std::abs(ratio - 1.0f) < 1e-4f;
        };

        bool any = c.record.hiddenBelowError > 0.0f && near(c.record.hiddenBelowError);

        for (std::uint32_t slot = 1; slot <= c.levels.levels; ++slot) {
            any = any || near(c.levels.errorMetres[slot]);
        }

        for (const auto& plane : c.frustum.planes) {
            const float signedDistance = plane[0] * c.record.bounds[0] + plane[1] * c.record.bounds[1] + plane[2] * c.record.bounds[2] + plane[3];
            any = any || std::abs(signedDistance + c.record.bounds[3]) < 1e-3f;
        }

        return any;
    }

    // Draws, meshes' levels, views and frustums all over a few kilometres, every flag and view option met.
    std::vector<Case> randomCases() {
        CaseRandom random{61};
        std::vector<Case> cases;

        for (int i = 0; i < 3000; ++i) {
            DrawItem draw;
            draw.mesh = MeshHandle{3};
            draw.bounds = i % 40 == 0 ? BoundingSphere{}
                                      : BoundingSphere{Vec3{random.between(-2000.0f, 2000.0f), random.between(0.0f, 300.0f), random.between(-2000.0f, 2000.0f)},
                                                       random.between(0.1f, 40.0f)};
            draw.hiddenBelowError = i % 9 == 0 ? random.between(0.05f, 2.0f) : 0.0f;
            draw.shadowOnly = i % 17 == 0;
            draw.hidden = i % 19 == 0;
            draw.material.alphaMode = i % 13 == 0 ? AlphaMode::Blend : i % 3 == 0 ? AlphaMode::Mask : AlphaMode::Opaque;
            draw.thinningEntry = i % 4 == 0 ? 1u : 0u;

            std::vector<MeshLevel> levels;
            float error = 0.01f;

            for (int level = 0; level < i % 4; ++level) {
                error *= random.between(2.0f, 8.0f);
                const bool drawsNothing = level == i % 4 - 1 && i % 5 == 0;
                levels.push_back(MeshLevel{drawsNothing ? MeshHandle{} : MeshHandle{10u + static_cast<std::uint32_t>(level)}, error, level % 2 == 0});
            }

            LevelOfDetailSettings settings;
            settings.enabled = i % 11 != 0;
            settings.impostors = i % 7 != 0;
            settings.pixelError = random.between(0.5f, 4.0f);
            LevelView view = levelView(Vec3{random.between(-1500.0f, 1500.0f), random.between(2.0f, 200.0f), random.between(-1500.0f, 1500.0f)},
                                       random.between(1080.0f, 2160.0f), random.between(0.6f, 1.4f), settings);
            view.orthographic = i % 10 == 0;

            const Vec3 eye = view.eye;
            const float yaw = random.between(0.0f, 6.28f);
            const Mat4 viewMatrix = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(random.between(0.6f, 1.4f), 16.0f / 9.0f, 0.1f, random.between(1000.0f, 5000.0f)));

            cases.push_back(Case{packCullRecord(draw), packMeshLevels(draw.mesh, levels), packLevelView(view),
                                 packFrustum(Frustum::fromClipMatrix(projection * viewMatrix, ClipDepth::OneToZero))});
        }

        return cases;
    }

    // The record, levels, view and frustum as the shader's thirteen inputs.
    void appendInputs(std::vector<GpuVec4>& inputs, const Case& c) {
        inputs.push_back(GpuVec4{c.record.bounds[0], c.record.bounds[1], c.record.bounds[2], c.record.bounds[3]});
        inputs.push_back(GpuVec4{c.record.levelSphere[0], c.record.levelSphere[1], c.record.levelSphere[2], c.record.levelSphere[3]});
        inputs.push_back(gpuVec4(static_cast<float>(c.record.flags), c.record.hiddenBelowError, static_cast<float>(c.levels.levels),
                                 static_cast<float>(c.levels.sharesVertices)));
        GpuVec4 meshes{};
        GpuVec4 errors{};

        for (int slot = 0; slot < CULL_LEVEL_SLOTS; ++slot) {
            meshes[slot] = c.levels.meshes[slot] == cullNoMesh ? -1.0f : static_cast<float>(c.levels.meshes[slot]);
            errors[slot] = c.levels.errorMetres[slot];
        }

        inputs.push_back(meshes);
        inputs.push_back(errors);
        inputs.push_back(GpuVec4{c.view.eye[0], c.view.eye[1], c.view.eye[2], c.view.eye[3]});
        inputs.push_back(gpuVec4(c.view.pixelError, static_cast<float>(c.view.flags)));

        for (const auto& plane : c.frustum.planes) {
            inputs.push_back(GpuVec4{plane[0], plane[1], plane[2], plane[3]});
        }
    }

    // The slot chosen, whether thinned, the sphere test and the camera's choice, against the C++ mirror, case by case.
    void checkDraws(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<Case> cases = randomCases();
        std::vector<GpuVec4> inputs;

        for (const Case& c : cases) {
            appendInputs(inputs, c);
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CULL_DRAW, inputs, 13, 1);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            if (nearAThreshold(cases[i])) {
                continue;
            }

            const Case& c = cases[i];
            const int slot = chooseLevelSlot(c.record, c.levels, c.view);
            report.check("chooseLevelSlot", exactTolerance, static_cast<float>(slot), outputs[i][0]);
            report.check("slotThinned", exactTolerance, slotThinned(c.record, c.levels, slot) ? 1.0f : 0.0f, outputs[i][1]);
            report.check("sphereInFrustum", exactTolerance, sphereInFrustum(c.frustum, c.record.bounds) ? 1.0f : 0.0f, outputs[i][2]);
            report.check("cameraTakes", exactTolerance, cameraTakes(c.record, c.frustum) ? 1.0f : 0.0f, outputs[i][3]);
        }
    }
}

void gpuTestCullRecords(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("culling.glsl, level_of_detail.glsl");
    checkDraws(runner, report);
}
