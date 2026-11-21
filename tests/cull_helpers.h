#pragma once

#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "scene/camera.h"
#include "scene/frame_description.h"
#include "scene/mesh_level.h"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

// Random scenes for GPU-driven drawing's tests (cull_records_test, camera_culling_test).

struct CullScene {
    std::vector<std::vector<MeshLevel>> levelsByMesh;
    std::vector<DrawItem> draws;
};

inline constexpr std::uint32_t cullSceneMeshes = 12;

/*
* Random draws over a few kilometres: meshes with no levels or up to three (the last sometimes drawing nothing, as a
* tree's does), some sharing the mesh's vertices; impostors; unknown bounds; shadow-only, hidden, glass, cut-out,
* double-sided and thinnable draws; a shared level sphere for some, as a tree's parts have; some swaying, some moved,
* some fading.
*/
inline CullScene randomCullScene(std::mt19937& random, int drawCount = 3000) {
    std::uniform_real_distribution<float> unit{0.0f, 1.0f};
    CullScene scene;

    for (std::uint32_t mesh = 0; mesh < cullSceneMeshes; ++mesh) {
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

    for (int i = 0; i < drawCount; ++i) {
        DrawItem draw;
        const std::uint32_t mesh = static_cast<std::uint32_t>(unit(random) * static_cast<float>(cullSceneMeshes)) % cullSceneMeshes;
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
        draw.shadowCascades = unit(random) < 0.2f ? 1 + static_cast<int>(unit(random) * 3.0f) : 0;
        // A plant's stand-in from a later cascade, a seen mesh handing its shadow on (by number, so the random stream keeps).
        draw.shadowFromCascade = i % 13 == 5 ? 1 + static_cast<int>(i % 2) : 0;
        draw.shadowHandedOn = i % 17 == 3;
        draw.material.alphaMode = unit(random) < 0.1f ? AlphaMode::Blend : unit(random) < 0.4f ? AlphaMode::Mask : AlphaMode::Opaque;
        draw.material.doubleSided = unit(random) < 0.3f;
        draw.thinningEntry = unit(random) < 0.3f ? 1 + static_cast<std::uint32_t>(i) : 0;
        draw.sway.kind = unit(random) < 0.4f ? SwayKind::Tree : SwayKind::Still;
        draw.fadesLevels = unit(random) < 0.4f;

        if (unit(random) < 0.1f) {
            draw.previousModel = unit(random) < 0.5f ? draw.model : Mat4::translation(0.0f, 0.1f, 0.0f);
        }

        scene.draws.push_back(draw);
    }

    return scene;
}

// The scene's level tables by mesh number.
inline std::vector<GpuMeshLevels> packCullSceneLevels(const CullScene& scene) {
    std::vector<GpuMeshLevels> tables;

    for (std::uint32_t mesh = 0; mesh < scene.levelsByMesh.size(); ++mesh) {
        tables.push_back(packMeshLevels(MeshHandle{mesh}, scene.levelsByMesh[mesh]));
    }

    return tables;
}

/*
* Both ways the passes can read a scene: every draw in flat groups (as moving draws are, one segment a group, no chunk),
* and every draw still in a kept table of chunks of chunkSlots draws (CameraCullTable), whose segments are culled by
* chunk first. Filled in place: the chunked frame points into the table.
*/
struct CullSceneTables {
    std::vector<GpuCullRecord> records;
    std::vector<GpuMeshLevels> levels;
    CullGroups flat;
    CullDrawOrder flatOrder;
    CameraCullTable table;
    CameraCullFrame chunked;
    CameraCullFrame::Joined joined;

    CullTables flatTables() const {
        return CullTables{records, levels, {}, flat.groups, flat.segments, flat.candidates};
    }

    CullTables chunkedTables() const {
        return chunked.tables(joined);
    }
};

inline void buildCullSceneTables(CullSceneTables& out, const CullScene& scene, const std::function<MeshPlacement(MeshHandle)>& placement,
                                 std::size_t chunkSlots = 16) {
    out.levels = packCullSceneLevels(scene);
    out.records.clear();

    for (const DrawItem& draw : scene.draws) {
        out.records.push_back(packCullRecord(draw));
    }

    out.flat = buildCullGroups(scene.draws, 0, scene.draws.size(), placement);
    out.flatOrder = cullDrawOrder(out.flat.styles);
    FrameDescription frame{.camera = Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f}};
    frame.draws = scene.draws;
    frame.stillDraws = scene.draws.size();
    frame.stillVersion = 1;
    frame.stillChunkSlots = chunkSlots;
    out.chunked = out.table.update(frame, placement, [](MeshHandle) { return false; });
    out.chunked.meshLevels = out.levels;
    out.joined = out.chunked.join();
}
