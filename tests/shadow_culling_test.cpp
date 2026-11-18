#include "cull_helpers.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/shadow_culling.h"
#include "scene/culling.h"
#include "scene/level_of_detail.h"

#include <cassert>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

namespace {
    // Every mesh's place: all pooled, a different place each.
    MeshPlacement shadowTestPlacement(MeshHandle mesh) {
        return MeshPlacement{1000 * mesh.index, 100, 5000 * mesh.index, 3 * (mesh.index + 1), true};
    }

    /*
    * A random tile over the test scene: a lamp's perspective face or a sun's box, a reach sphere (sometimes unknown), a
    * smallest caster, a cascade (or none) and sometimes its movers kept apart.
    */
    ShadowTile randomTile(std::mt19937& random) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        ShadowTile tile;
        const Vec3 place{(unit(random) - 0.5f) * 3000.0f, 20.0f + unit(random) * 200.0f, (unit(random) - 0.5f) * 3000.0f};
        const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
        const Vec3 forward{std::cos(yaw), -0.3f - unit(random), std::sin(yaw)};

        if (unit(random) < 0.5f) {
            const float half = 50.0f + unit(random) * 800.0f;
            tile.matrix = toGpuDepthRange(Mat4::orthographic(-half, half, -half, half, 0.0f, 1500.0f)) * Mat4::lookAt(place, place + forward, Vec3{0.0f, 1.0f, 0.0f});
            tile.lightPlace = Vec4{-forward.x, -forward.y, -forward.z, 0.0f};
        } else {
            tile.matrix = toGpuDepthRange(Mat4::perspective(0.5f * std::numbers::pi_v<float>, 1.0f, 0.1f, 300.0f + unit(random) * 600.0f)) *
                          Mat4::lookAt(place, place + forward, Vec3{0.0f, 1.0f, 0.0f});
            tile.lightPlace = Vec4{place.x, place.y, place.z, 1.0f};
        }

        tile.size = unit(random) < 0.5f ? 1024 : 4096;
        tile.reach = unit(random) < 0.2f ? BoundingSphere{} : BoundingSphere{place, 200.0f + unit(random) * 1500.0f};
        tile.smallestCaster = unit(random) < 0.5f ? 0.0f : unit(random) * 20.0f;
        tile.sunCascade = unit(random) < 0.4f ? -1 : static_cast<int>(unit(random) * 4.0f);
        tile.separateMovers = unit(random) < 0.3f;
        return tile;
    }

    std::vector<std::uint32_t> orderOf(const BatchedDraws& batched) {
        return std::vector<std::uint32_t>(batched.order.begin(), batched.order.end());
    }

    /*
    * Each tile as a view of the passes' steps gives the CPU's casters (castersInReach, then shadowTileCasters), levels by
    * the tile's texels, batched as batchByMesh orders them, with no thinned bits (shadow shaders thin nothing); a tile
    * keeping its movers apart lists its still casters first and those that moved second, as splitMovingCasters does.
    */
    void testShadowListsMatchTheCpu() {
        std::mt19937 random{71};
        const CullScene scene = randomCullScene(random);
        CullSceneTables both;
        buildCullSceneTables(both, scene, shadowTestPlacement);
        std::size_t drawn = 0;
        std::size_t moved = 0;

        for (int index = 0; index < 40; ++index) {
            const ShadowTile tile = randomTile(random);
            LevelOfDetailSettings settings;
            settings.enabled = index % 5 != 4;
            const std::vector<std::size_t> found = shadowTileCasters(scene.draws, castersInReach(scene.draws, tile.reach), tile);
            const ShadowCasterSplit split = tile.separateMovers ? splitMovingCasters(scene.draws, found) : ShadowCasterSplit{found, {}};
            const LevelView levels = tileLevelView(tile, settings);

            // Through the flat groups and the chunked table alike.
            for (int form = 0; form < 2; ++form) {
                const CullTables tables = form == 0 ? both.flatTables() : both.chunkedTables();
                const CullDrawOrder& order = form == 0 ? both.flatOrder : both.chunked.drawOrder;
                GpuCameraCull view = packShadowCull(tile, settings);
                assert((view.flags & CAMERA_CULL_SHADOW) != 0u);
                assert(((view.flags & CAMERA_CULL_MOTION) != 0u) == tile.separateMovers);
                placeCullView(view, 0, static_cast<std::uint32_t>(tables.candidates.size()), static_cast<std::uint32_t>(tables.segments.size()), order);
                view.mainBase = 0;
                view.motionBase = 0;
                const CameraCullResult result = cullCameraOnCpu(tables, order.order, view);
                assert(result.mainEntries == orderOf(batchByMesh(scene.draws, split.still, 0, &levels)));
                assert(result.motionEntries == orderOf(batchByMesh(scene.draws, split.moving, 0, &levels)));
            }

            drawn += split.still.size();
            moved += split.moving.size();
        }

        assert(drawn > 1000 && moved > 0);
    }

    // A frame of the test scene: its first stillDraws kept from the frame before (version), built ranges announced.
    FrameDescription frameOf(const std::vector<DrawItem>& draws, std::size_t stillDraws, std::uint64_t version, std::uint64_t previous,
                             std::vector<DrawRange> built = {}) {
        FrameDescription frame{.camera = Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f}};
        frame.draws = draws;
        frame.stillDraws = stillDraws;
        frame.stillVersion = version;
        frame.stillPreviousVersion = previous;
        frame.builtStillDraws = std::move(built);
        return frame;
    }

    // Today's rule: the tile's fingerprint over all its still casters (shadow_cache), the reference the new rule must cover.
    std::uint64_t fingerprint(const std::vector<DrawItem>& draws, const ShadowTile& tile, const LevelOfDetailSettings& settings) {
        const std::vector<std::size_t> found = shadowTileCasters(draws, castersInReach(draws, tile.reach), tile);
        const ShadowCasterSplit split = tile.separateMovers ? splitMovingCasters(draws, found) : ShadowCasterSplit{found, {}};
        const LevelView levels = tileLevelView(tile, settings);
        return tintedShadowSignature(draws, split.still, {}, &levels);
    }

    /*
    * Over frames of changes (still draws rebuilt in place and moved, movers moving and standing, nothing at all, the
    * still draws rebuilt whole), every tile whose fingerprint changed is redrawn by the announced-change rule; with
    * nothing changed none is; after a whole rebuild all are; a tile that sways is redrawn every frame.
    */
    void testShadowCacheCoversTheFingerprint() {
        std::mt19937 random{73};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        CullScene scene = randomCullScene(random, 1500);
        constexpr std::size_t stillDraws = 1200;

        // Still draws never move between their builds.
        for (std::size_t draw = 0; draw < stillDraws; ++draw) {
            scene.draws[draw].previousModel.reset();
        }

        std::vector<ShadowTile> tiles;

        for (int index = 0; index < 24; ++index) {
            ShadowTile tile = randomTile(random);
            tile.x = static_cast<std::uint32_t>(index) * 4096;
            tiles.push_back(tile);
        }

        const LevelOfDetailSettings settings;
        CameraCullTable table;
        ShadowTileMemory memory;
        ShadowTileMemory reference;
        std::uint64_t frameNumber = 0;
        std::size_t covered = 0;

        // One frame: each tile's decision by both rules, the new one covering the old; both remember what they drew.
        const auto runFrame = [&](const FrameDescription& frame, bool expectAll, bool expectNone) {
            const CameraCullFrame culled = table.update(frame, shadowTestPlacement, [](MeshHandle) { return false; });
            ++frameNumber;

            for (const ShadowTile& tile : tiles) {
                const ShadowTileNeeds needs = shadowTileNeeds(tile, settings, culled, frame.draws, {}, false, frameNumber, memory);
                const std::uint64_t print = fingerprint(frame.draws, tile, settings);
                const bool fingerprintChanged = !reference.isCurrent(tile, print);
                assert(!fingerprintChanged || needs.redraw);
                assert(!expectAll || needs.redraw);
                assert(!expectNone || !needs.redraw);
                covered += fingerprintChanged ? 1 : 0;

                if (needs.redraw) {
                    memory.remember(tile, needs.key);
                }

                reference.remember(tile, print);
            }
        };

        runFrame(frameOf(scene.draws, stillDraws, 1, 0), true, false);

        for (std::uint64_t version = 2; version < 8; ++version) {
            std::vector<DrawRange> built;

            // A few still draws rebuilt where they stood or moved (by up to 300 m), announced.
            for (int change = 0; change < 6; ++change) {
                const std::size_t draw = static_cast<std::size_t>(unit(random) * static_cast<float>(stillDraws)) % stillDraws;
                DrawItem& item = scene.draws[draw];

                if (unit(random) < 0.7f && item.bounds.radius > 0.0f) {
                    const Vec3 step{(unit(random) - 0.5f) * 600.0f, 0.0f, (unit(random) - 0.5f) * 600.0f};
                    item.bounds.centre = item.bounds.centre + step;
                    item.model = Mat4::translation(step.x, step.y, step.z) * item.model;
                }

                built.push_back(DrawRange{draw, 1});
            }

            // Movers: some move (their last frame's matrix where they were), the rest stand.
            for (std::size_t draw = stillDraws; draw < scene.draws.size(); ++draw) {
                DrawItem& item = scene.draws[draw];
                item.previousModel = item.model;

                if (unit(random) < 0.2f && item.bounds.radius > 0.0f) {
                    const Vec3 step{(unit(random) - 0.5f) * 100.0f, 0.0f, (unit(random) - 0.5f) * 100.0f};
                    item.bounds.centre = item.bounds.centre + step;
                    item.model = Mat4::translation(step.x, step.y, step.z) * item.model;
                }
            }

            runFrame(frameOf(scene.draws, stillDraws, version, version - 1, built), false, false);
        }

        // Nothing changed: the movers stand, no still draw was built.
        for (std::size_t draw = stillDraws; draw < scene.draws.size(); ++draw) {
            scene.draws[draw].previousModel = scene.draws[draw].model;
        }

        runFrame(frameOf(scene.draws, stillDraws, 8, 7), false, false);
        runFrame(frameOf(scene.draws, stillDraws, 9, 8), false, true);

        // Rebuilt whole (the version does not follow): every tile.
        runFrame(frameOf(scene.draws, stillDraws, 20, 15), true, false);
        assert(covered > 24);

        // A swaying tile's key is new every frame.
        const CameraCullFrame culled = table.update(frameOf(scene.draws, stillDraws, 21, 20), shadowTestPlacement, [](MeshHandle) { return false; });
        const ShadowTileNeeds first = shadowTileNeeds(tiles[0], settings, culled, scene.draws, {}, true, 100, memory);
        memory.remember(tiles[0], first.key);
        assert(shadowTileNeeds(tiles[0], settings, culled, scene.draws, {}, true, 101, memory).redraw);
    }

    // A caster's sphere touching the reach or exactly the smallest caster's size is near an edge; well inside, not.
    void testShadowEdges() {
        ShadowTile tile;
        tile.matrix = toGpuDepthRange(Mat4::perspective(1.0f, 1.0f, 0.1f, 1000.0f));
        tile.lightPlace = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        tile.reach = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 101.0f};
        tile.smallestCaster = 0.5f;

        DrawItem draw;
        draw.mesh = MeshHandle{0};
        draw.bounds = BoundingSphere{Vec3{0.0f, 0.0f, -50.0f}, 2.0f};
        const GpuMeshLevels levels = packMeshLevels(draw.mesh, {});
        const LevelOfDetailSettings settings;
        assert(!nearACameraEdge(packCullRecord(draw), levels, packShadowCull(tile, settings)));

        draw.bounds = BoundingSphere{Vec3{0.0f, 0.0f, -100.0f}, 1.0f};
        tile.reach = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 99.0f};
        assert(nearACameraEdge(packCullRecord(draw), levels, packShadowCull(tile, settings)));

        tile.reach = BoundingSphere{Vec3{0.0f, 0.0f, 0.0f}, 500.0f};
        tile.smallestCaster = 1.0f;
        assert(nearACameraEdge(packCullRecord(draw), levels, packShadowCull(tile, settings)));
    }
}

/*
* Shadow tiles as views of the GPU culling (render/gpu/shadow_culling.h): their lists equal the CPU's casters, and the
* cache's announced-change rule redraws every tile the casters' fingerprint would.
*/
void testShadowCulling() {
    testShadowListsMatchTheCpu();
    testShadowCacheCoversTheFingerprint();
    testShadowEdges();
}
