#include "cull_helpers.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/transparency.h"
#include "scene/culling.h"
#include "scene/level_of_detail.h"

#include <cassert>
#include <cstddef>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
    // The mesh a slot names, as chosenMesh answers: none for no slot.
    MeshHandle slotMesh(const GpuMeshLevels& levels, int slot) {
        return slot == cullNoSlot ? MeshHandle{} : MeshHandle{levels.meshes[slot]};
    }

    /*
    * Choosing from the packed records gives the CPU's mesh for every draw, in perspective views near and far, with
    * levels and impostors on and off, and orthographic (the sun's tiles), drawing cluster levels or not; thins exactly
    * the draws batchByMesh would; and fades the draws levelFade fades, with or without a band.
    */
    void testLevelsMatchTheCpu() {
        std::mt19937 random{29};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const CullScene scene = randomCullScene(random);
        const std::vector<GpuMeshLevels> tables = packCullSceneLevels(scene);
        int drawnNothing = 0;
        int coarser = 0;
        int fading = 0;

        for (int viewIndex = 0; viewIndex < 24; ++viewIndex) {
            LevelOfDetailSettings settings;
            settings.enabled = viewIndex % 6 != 5;
            settings.impostors = viewIndex % 4 != 3;
            settings.pixelError = 0.5f + unit(random) * 4.0f;
            settings.fadeBand = viewIndex % 3 == 2 ? 0.0f : 0.05f + unit(random) * 0.5f;
            LevelView view = cameraLevelView(Vec3{(unit(random) - 0.5f) * 3000.0f, 2.0f + unit(random) * 200.0f, (unit(random) - 0.5f) * 3000.0f},
                                             1080.0f + unit(random) * 1080.0f, 0.6f + unit(random) * 0.8f, settings);
            view.orthographic = viewIndex % 5 == 4;
            view.clusterLevels = viewIndex % 2 == 1;

            if (view.orthographic) {
                view.pixelsPerMetre = 0.5f + unit(random) * 40.0f;
            }

            const GpuLevelView packedView = packLevelView(view);

            for (const DrawItem& draw : scene.draws) {
                const GpuCullRecord record = packCullRecord(draw);
                const GpuMeshLevels& levels = tables[draw.mesh.index];
                const int slot = chooseLevelSlot(record, levels, packedView);
                assert(slotMesh(levels, slot) == chosenMesh(draw, view));

                const MeshHandle mesh = chosenMesh(draw, view);
                bool expectedThinned = false;

                if (mesh.isValid() && draw.thinningEntry != 0) {
                    expectedThinned = mesh == draw.mesh;

                    for (const MeshLevel& level : draw.levels) {
                        expectedThinned = expectedThinned || (level.mesh == mesh && level.sharesVertices);
                    }
                }

                assert(slotThinned(record, levels, slot) == expectedThinned);

                // Fading as the CPU fades it: the same two meshes (a level without one, none) and the same share.
                const LevelFade fade = levelFade(draw, view);
                const GpuLevelFade packedFade = levelFadeSlots(record, levels, packedView);
                assert(packedFade.fading == fade.fading);

                if (fade.fading) {
                    assert(slotMesh(levels, packedFade.finer) == fade.finer && slotMesh(levels, packedFade.coarser) == fade.coarser);
                    assert(packedFade.finer == slot && packedFade.share == fade.share);
                    fading += 1;
                }

                drawnNothing += slot == cullNoSlot ? 1 : 0;
                coarser += slot > 0 ? 1 : 0;
            }
        }

        // The cases were really met: draws drawing nothing, draws drawn coarser, draws fading.
        assert(drawnNothing > 1000 && coarser > 1000 && fading > 100);
    }

    /*
    * The sphere test equals Frustum::intersectsSphere, and the camera takes exactly the draws visibleDraws then
    * opaqueDraws keep, in a few cameras looking about.
    */
    void testFrustumMatchesTheCpu() {
        std::mt19937 random{31};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const CullScene scene = randomCullScene(random);

        for (int viewIndex = 0; viewIndex < 8; ++viewIndex) {
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.6f + unit(random), 16.0f / 9.0f, 0.1f, 1000.0f + unit(random) * 4000.0f));
            const Vec3 eye{(unit(random) - 0.5f) * 2000.0f, 10.0f + unit(random) * 100.0f, (unit(random) - 0.5f) * 2000.0f};
            const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
            const Mat4 view = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            const Frustum frustum = Frustum::fromClipMatrix(projection * view, ClipDepth::OneToZero);
            const GpuFrustum packed = packFrustum(frustum);

            const std::vector<std::size_t> expected = opaqueDraws(scene.draws, visibleDraws(scene.draws, frustum));
            std::vector<std::size_t> taken;

            for (std::size_t index = 0; index < scene.draws.size(); ++index) {
                const DrawItem& draw = scene.draws[index];
                const GpuCullRecord record = packCullRecord(draw);
                assert(sphereInFrustum(packed, record.bounds) == (draw.bounds.radius < 0.0f || frustum.intersectsSphere(draw.bounds.centre, draw.bounds.radius)));

                if (cameraTakes(record, packed)) {
                    taken.push_back(index);
                }
            }

            assert(taken == expected);
            assert(!taken.empty() && taken.size() < scene.draws.size());
        }
    }

    // The records carry each fact the views choose by; a mesh with four levels is refused.
    void testRecordsCarryTheDraw() {
        DrawItem draw;
        draw.mesh = MeshHandle{7};
        draw.bounds = BoundingSphere{Vec3{1.0f, 2.0f, 3.0f}, 4.0f};
        draw.shadowOnly = true;
        draw.castsShadows = true;
        draw.material.alphaMode = AlphaMode::Mask;
        draw.thinningEntry = 5;
        draw.hiddenBelowError = 0.25f;
        const GpuCullRecord record = packCullRecord(draw);
        assert(record.bounds[0] == 1.0f && record.bounds[3] == 4.0f);
        assert(record.levelSphere[3] == 4.0f); // no level sphere of its own: its bounds
        assert(record.flags == (CULL_SHADOW_ONLY | CULL_CASTS_SHADOWS | CULL_CUT_OUT | CULL_THINNABLE));
        assert(record.mesh == 7 && record.hiddenBelowError == 0.25f);

        // Moved (a last-frame matrix of its own) and swaying; a last-frame matrix equal to its own is not a move.
        DrawItem moving;
        moving.sway.kind = SwayKind::Grass;
        moving.previousModel = Mat4::translation(1.0f, 0.0f, 0.0f);
        assert(packCullRecord(moving).flags == (CULL_CASTS_SHADOWS | CULL_MOVED | CULL_SWAYS));
        moving.previousModel = moving.model;
        assert(packCullRecord(moving).flags == (CULL_CASTS_SHADOWS | CULL_SWAYS));

        // Fading levels (DrawItem::fadesLevels).
        moving.fadesLevels = true;
        assert(packCullRecord(moving).flags == (CULL_CASTS_SHADOWS | CULL_SWAYS | CULL_FADES));

        // Cluster levels of its own (DrawItem::clusterLevels), and a view drawing them (LevelView::clusterLevels).
        moving.clusterLevels = true;
        assert(packCullRecord(moving).flags == (CULL_CASTS_SHADOWS | CULL_SWAYS | CULL_FADES | CULL_CLUSTER_LEVELS));
        LevelView clusterView;
        assert((packLevelView(clusterView).flags & CULL_LEVELS_CLUSTERS) == 0u);
        clusterView.clusterLevels = true;
        assert((packLevelView(clusterView).flags & CULL_LEVELS_CLUSTERS) != 0u);

        const std::vector<MeshLevel> four(4, MeshLevel{MeshHandle{1}, 0.1f, false});
        bool refused = false;

        try {
            packMeshLevels(MeshHandle{0}, four);
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);
    }
}

/*
* GPU-driven drawing's records (render/gpu/cull_records.h): choosing from them gives the CPU's answers, draw for draw.
*/
void testCullRecords() {
    testLevelsMatchTheCpu();
    testFrustumMatchesTheCpu();
    testRecordsCarryTheDraw();
}
