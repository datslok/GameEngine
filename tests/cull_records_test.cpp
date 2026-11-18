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
    struct Scene {
        std::vector<std::vector<MeshLevel>> levelsByMesh;
        std::vector<DrawItem> draws;
    };

    /*
    * Random draws over a few kilometres: meshes with no levels or up to three (the last sometimes drawing nothing, as a
    * tree's does), some sharing the mesh's vertices; impostors; unknown bounds; shadow-only, hidden, glass, cut-out,
    * double-sided and thinnable draws; a shared level sphere for some, as a tree's parts have.
    */
    Scene randomScene(std::mt19937& random) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        Scene scene;

        for (std::uint32_t mesh = 0; mesh < 12; ++mesh) {
            std::vector<MeshLevel> levels;
            const int count = static_cast<int>(unit(random) * 4.0f);
            float error = 0.01f;

            for (int level = 0; level < count; ++level) {
                error *= 2.0f + 6.0f * unit(random);
                const bool drawsNothing = level == count - 1 && unit(random) < 0.4f;
                levels.push_back(MeshLevel{drawsNothing ? MeshHandle{} : MeshHandle{100 + mesh * 4 + static_cast<std::uint32_t>(level)}, error,
                                           unit(random) < 0.5f});
            }

            scene.levelsByMesh.push_back(levels);
        }

        for (int i = 0; i < 3000; ++i) {
            DrawItem draw;
            const std::uint32_t mesh = static_cast<std::uint32_t>(unit(random) * 12.0f) % 12;
            draw.mesh = MeshHandle{mesh};
            draw.levels = scene.levelsByMesh[mesh];
            draw.bounds = unit(random) < 0.03f ? BoundingSphere{}
                                               : BoundingSphere{Vec3{(unit(random) - 0.5f) * 4000.0f, unit(random) * 300.0f, (unit(random) - 0.5f) * 4000.0f},
                                                                0.1f + unit(random) * 40.0f};

            if (unit(random) < 0.3f && draw.bounds.radius > 0.0f) {
                draw.levelBounds = BoundingSphere{draw.bounds.centre + Vec3{3.0f, 5.0f, -2.0f}, draw.bounds.radius * 3.0f};
            }

            draw.hiddenBelowError = unit(random) < 0.1f ? 0.05f + unit(random) * 2.0f : 0.0f;
            draw.shadowOnly = unit(random) < 0.05f;
            draw.hidden = unit(random) < 0.05f;
            draw.castsShadows = unit(random) < 0.8f;
            draw.material.alphaMode = unit(random) < 0.1f ? AlphaMode::Blend : unit(random) < 0.4f ? AlphaMode::Mask : AlphaMode::Opaque;
            draw.material.doubleSided = unit(random) < 0.3f;
            draw.thinningEntry = unit(random) < 0.3f ? 1 + static_cast<std::uint32_t>(i) : 0;
            scene.draws.push_back(draw);
        }

        return scene;
    }

    std::vector<GpuMeshLevels> packTables(const Scene& scene) {
        std::vector<GpuMeshLevels> tables;

        for (std::uint32_t mesh = 0; mesh < scene.levelsByMesh.size(); ++mesh) {
            tables.push_back(packMeshLevels(MeshHandle{mesh}, scene.levelsByMesh[mesh]));
        }

        return tables;
    }

    // The mesh a slot names, as chosenMesh answers: none for no slot.
    MeshHandle slotMesh(const GpuMeshLevels& levels, int slot) {
        return slot == cullNoSlot ? MeshHandle{} : MeshHandle{levels.meshes[slot]};
    }

    /*
    * Choosing from the packed records gives the CPU's mesh for every draw, in perspective views near and far, with
    * levels and impostors on and off, and orthographic (the sun's tiles); and thins exactly the draws batchByMesh would.
    */
    void testLevelsMatchTheCpu() {
        std::mt19937 random{29};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const Scene scene = randomScene(random);
        const std::vector<GpuMeshLevels> tables = packTables(scene);
        int drawnNothing = 0;
        int coarser = 0;

        for (int viewIndex = 0; viewIndex < 24; ++viewIndex) {
            LevelOfDetailSettings settings;
            settings.enabled = viewIndex % 6 != 5;
            settings.impostors = viewIndex % 4 != 3;
            settings.pixelError = 0.5f + unit(random) * 4.0f;
            LevelView view = levelView(Vec3{(unit(random) - 0.5f) * 3000.0f, 2.0f + unit(random) * 200.0f, (unit(random) - 0.5f) * 3000.0f},
                                       1080.0f + unit(random) * 1080.0f, 0.6f + unit(random) * 0.8f, settings);
            view.orthographic = viewIndex % 5 == 4;

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
                drawnNothing += slot == cullNoSlot ? 1 : 0;
                coarser += slot > 0 ? 1 : 0;
            }
        }

        // The cases were really met: draws drawing nothing and draws drawn coarser.
        assert(drawnNothing > 1000 && coarser > 1000);
    }

    /*
    * The sphere test equals Frustum::intersectsSphere, and the camera takes exactly the draws visibleDraws then
    * opaqueDraws keep, in a few cameras looking about.
    */
    void testFrustumMatchesTheCpu() {
        std::mt19937 random{31};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const Scene scene = randomScene(random);

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
