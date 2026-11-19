#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_depth_range.h"

#include <algorithm>
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

        // The camera's own (camera_culling.glsl): the slot a candidate names, the sway's reach and the view's flags.
        std::uint32_t slot = 0;
        float swayFadeEnd = 0.0f;
        std::uint32_t cameraFlags = 0;

        // A shadow tile's (shadowTakes): its reach, smallest caster, cascade and thinning band.
        float reach[4] = {0.0f, 0.0f, 0.0f, -1.0f};
        float smallestCaster = 0.0f;
        std::int32_t cascade = -1;
        float thinning[2] = {0.0f, 0.0f};
    };

    // The case's camera record (GpuCameraCull) for cameraCandidateFlags.
    GpuCameraCull cameraOf(const Case& c) {
        GpuCameraCull view;
        view.frustum = c.frustum;
        view.levels = c.view;
        view.swayFadeEnd = c.swayFadeEnd;
        view.flags = c.cameraFlags;
        std::copy(std::begin(c.reach), std::end(c.reach), std::begin(view.reach));
        view.smallestCaster = c.smallestCaster;
        view.cascade = c.cascade;
        view.thinning[0] = c.thinning[0];
        view.thinning[1] = c.thinning[1];
        return view;
    }

    /*
    * Whether any of the case's level tests lands within a hair of its threshold, where the GPU's square root may round
    * the other way: a choice either side is right there, so those cases are left out of the comparison.
    */
    bool nearAThreshold(const Case& c) {
        return nearACameraEdge(c.record, c.levels, cameraOf(c));
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
            draw.sway.kind = i % 3 == 1 ? SwayKind::Plant : SwayKind::Still;
            draw.castsShadows = i % 7 != 3;
            draw.shadowCascades = i % 5 == 1 ? 1 + i % 3 : 0;
            draw.fadesLevels = i % 3 != 2;

            if (i % 8 == 2) {
                draw.previousModel = Mat4::translation(0.0f, 1.0f, 0.0f);
            }

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
            settings.fadeBand = i % 4 == 3 ? 0.0f : random.between(0.05f, 0.5f);
            LevelView view = cameraLevelView(Vec3{random.between(-1500.0f, 1500.0f), random.between(2.0f, 200.0f), random.between(-1500.0f, 1500.0f)},
                                             random.between(1080.0f, 2160.0f), random.between(0.6f, 1.4f), settings);
            view.orthographic = i % 10 == 0;

            const Vec3 eye = view.eye;
            const float yaw = random.between(0.0f, 6.28f);
            const Mat4 viewMatrix = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(random.between(0.6f, 1.4f), 16.0f / 9.0f, 0.1f, random.between(1000.0f, 5000.0f)));

            Case c{packCullRecord(draw), packMeshLevels(draw.mesh, levels), packLevelView(view),
                   packFrustum(Frustum::fromClipMatrix(projection * viewMatrix, ClipDepth::OneToZero))};
            c.levels.changesShape = i % 23 == 0 ? 1u : 0u;
            c.slot = static_cast<std::uint32_t>(i / 4) % 4u;
            c.swayFadeEnd = random.between(20.0f, 3000.0f);
            c.cameraFlags = (i % 6 != 0 ? CAMERA_CULL_MOTION : 0u) | (i % 5 != 0 ? CAMERA_CULL_SWAY : 0u) | (i % 3 == 0 ? CAMERA_CULL_SHADOW : 0u);

            if (i % 4 != 0) {
                const Vec3 reachCentre{random.between(-1500.0f, 1500.0f), random.between(0.0f, 200.0f), random.between(-1500.0f, 1500.0f)};
                const float reachRadius = random.between(100.0f, 2500.0f);
                c.reach[0] = reachCentre.x;
                c.reach[1] = reachCentre.y;
                c.reach[2] = reachCentre.z;
                c.reach[3] = reachRadius;
            }

            c.smallestCaster = i % 2 == 0 ? random.between(0.0f, 20.0f) : 0.0f;
            c.cascade = i % 3 == 0 ? -1 : static_cast<std::int32_t>(i % 4);

            if (i % 7 != 0) {
                c.thinning[0] = random.between(0.3f, 0.9f);
                c.thinning[1] = random.between(0.0f, 0.25f);
            }

            cases.push_back(c);
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
        inputs.push_back(gpuVec4(c.view.pixelError, static_cast<float>(c.view.flags), c.view.fadeBand));

        for (const auto& plane : c.frustum.planes) {
            inputs.push_back(GpuVec4{plane[0], plane[1], plane[2], plane[3]});
        }
    }

    // A candidate's flags, the sway's reach, the shadow tiles' test and its fade field, against the C++ mirror, case by
    // case (the draw cases plus four inputs).
    void checkCandidates(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<Case> cases = randomCases();
        std::vector<GpuVec4> inputs;

        for (const Case& c : cases) {
            appendInputs(inputs, c);
            inputs.push_back(gpuVec4(static_cast<float>(c.levels.changesShape), static_cast<float>(c.slot), c.swayFadeEnd, static_cast<float>(c.cameraFlags)));
            inputs.push_back(GpuVec4{c.reach[0], c.reach[1], c.reach[2], c.reach[3]});
            inputs.push_back(gpuVec4(c.smallestCaster, static_cast<float>(c.cascade), static_cast<float>(c.record.shadowCascades)));
            inputs.push_back(gpuVec4(c.thinning[0], c.thinning[1]));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CAMERA_CANDIDATE, inputs, 17, 1);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            if (nearAThreshold(cases[i])) {
                continue;
            }

            const Case& c = cases[i];
            const GpuCameraCull view = cameraOf(c);
            const Vec3 eye{c.view.eye[0], c.view.eye[1], c.view.eye[2]};
            report.check("cameraCandidateFlags", exactTolerance, static_cast<float>(cameraCandidateFlags(c.record, c.levels, view, c.slot)), outputs[i][0]);
            report.check("swayReaches", exactTolerance, swayReaches(c.record.bounds, eye, c.swayFadeEnd) ? 1.0f : 0.0f, outputs[i][1]);
            report.check("shadowTakes", exactTolerance, shadowTakes(c.record, view) ? 1.0f : 0.0f, outputs[i][2]);
            report.check("cameraCandidateFade", exactTolerance, static_cast<float>(cameraCandidateFade(c.record, c.levels, view, c.slot)), outputs[i][3]);
        }
    }

    /*
    * Whether a box's test lands within a centimetre of a plane or of the light's reach, where rounding may go either way:
    * those cases are left out.
    */
    bool chunkNearAnEdge(const GpuCullChunk& chunk, const GpuCameraCull& view) {
        constexpr float edge = 0.01f;

        for (const auto& plane : view.frustum.planes) {
            const float x = plane[0] >= 0.0f ? chunk.maximum[0] : chunk.minimum[0];
            const float y = plane[1] >= 0.0f ? chunk.maximum[1] : chunk.minimum[1];
            const float z = plane[2] >= 0.0f ? chunk.maximum[2] : chunk.minimum[2];

            if (std::abs(plane[0] * x + plane[1] * y + plane[2] * z + plane[3]) < edge) {
                return true;
            }
        }

        float distanceSquared = 0.0f;

        for (int axis = 0; axis < 3; ++axis) {
            const float gap = view.reach[axis] - std::clamp(view.reach[axis], chunk.minimum[axis], chunk.maximum[axis]);
            distanceSquared += gap * gap;
        }

        return view.reach[3] >= 0.0f && std::abs(std::sqrt(distanceSquared) - view.reach[3]) < edge;
    }

    // A chunk round each case's record (with its flags), against the C++ mirror's chunkInView and boxInFrustum.
    void checkChunks(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<Case> cases = randomCases();
        std::vector<GpuVec4> inputs;
        std::vector<GpuCullChunk> chunks;

        for (const Case& c : cases) {
            const GpuCullChunk chunk = packCullChunk(std::span<const GpuCullRecord>{&c.record, 1});
            chunks.push_back(chunk);
            inputs.push_back(GpuVec4{chunk.minimum[0], chunk.minimum[1], chunk.minimum[2], static_cast<float>(chunk.flags)});
            inputs.push_back(GpuVec4{chunk.maximum[0], chunk.maximum[1], chunk.maximum[2], 0.0f});

            for (const auto& plane : c.frustum.planes) {
                inputs.push_back(GpuVec4{plane[0], plane[1], plane[2], plane[3]});
            }

            inputs.push_back(GpuVec4{c.reach[0], c.reach[1], c.reach[2], c.reach[3]});
            inputs.push_back(gpuVec4(static_cast<float>(c.cameraFlags)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CHUNK_IN_VIEW, inputs, 10, 1);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            const GpuCameraCull view = cameraOf(cases[i]);

            if (chunkNearAnEdge(chunks[i], view)) {
                continue;
            }

            report.check("chunkInView", exactTolerance, chunkInView(chunks[i], view) ? 1.0f : 0.0f, outputs[i][0]);
            report.check("boxInFrustum", exactTolerance, boxInFrustum(view.frustum, chunks[i].minimum, chunks[i].maximum) ? 1.0f : 0.0f, outputs[i][1]);
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

namespace {
    // Each draw's fade (both slots and the share), against the C++ mirror; the share as close as the distance's square
    // root rounds (a few parts in a million of the band's width).
    void checkFadeSlots(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<Case> cases = randomCases();
        std::vector<GpuVec4> inputs;

        for (const Case& c : cases) {
            appendInputs(inputs, c);
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_LEVEL_FADE_SLOTS, inputs, 13, 1);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            if (nearAThreshold(cases[i])) {
                continue;
            }

            const GpuLevelFade fade = levelFadeSlots(cases[i].record, cases[i].levels, cases[i].view);
            report.check("levelFadeSlots fading", exactTolerance, fade.fading ? 1.0f : 0.0f, outputs[i][0]);
            report.check("levelFadeSlots finer", exactTolerance, static_cast<float>(fade.finer), outputs[i][1]);
            report.check("levelFadeSlots coarser", exactTolerance, static_cast<float>(fade.coarser), outputs[i][2]);
            report.check("levelFadeSlots share", FloatTolerance{.absolute = 1e-4f, .relative = 0.0f}, fade.share, outputs[i][3]);
        }
    }

    // Entries of draws up to the instance limit, with and without the thinned bit, and every fade field.
    void checkEntries(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<std::uint32_t> expected;

        for (std::uint32_t field = 0; field < 128u; ++field) {
            const std::uint32_t draw = (field * 131071u) % 16777216u;
            const std::uint32_t flags = field % 2 == 0 ? CAMERA_CULL_DRAWN : CAMERA_CULL_DRAWN | CAMERA_CULL_THINNED | CAMERA_CULL_FADES;
            inputs.push_back(gpuVec4(static_cast<float>(draw), static_cast<float>(flags), static_cast<float>(field)));
            expected.push_back(cameraCandidateEntry(draw, flags, field));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CANDIDATE_ENTRY, inputs, 1, 1);

        for (std::size_t i = 0; i < expected.size(); ++i) {
            report.check("cameraCandidateEntry low", exactTolerance, static_cast<float>(expected[i] & 0xffffu), outputs[i][0]);
            report.check("cameraCandidateEntry high", exactTolerance, static_cast<float>(expected[i] >> 16u), outputs[i][1]);
        }
    }
}

void gpuTestCullRecords(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("culling.glsl, level_of_detail.glsl");
    checkDraws(runner, report);
    checkFadeSlots(runner, report);
    report.section("camera_culling.glsl");
    checkCandidates(runner, report);
    checkChunks(runner, report);
    checkEntries(runner, report);
}
