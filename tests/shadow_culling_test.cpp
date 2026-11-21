#include "cull_helpers.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/shadow_culling.h"
#include "scene/culling.h"
#include "scene/level_of_detail.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
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

        if (unit(random) < 0.7f) {
            tile.thinStart = 0.3f + unit(random) * 0.6f;
            tile.thinEnd = unit(random) * 0.25f;
            tile.thinBall = BoundingSphere{place + forward * 300.0f, 50.0f + unit(random) * 400.0f};
        }

        return tile;
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

    // A sun cascade's box straight down over place, half wide, its ball (the camera's) round ball.
    ShadowTile cascadeDownOver(const Vec3& place, float half, const BoundingSphere& ball) {
        ShadowTile tile;
        tile.matrix = toGpuDepthRange(Mat4::orthographic(-half, half, -half, half, 0.0f, 1500.0f)) *
                      Mat4::lookAt(place, place + Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
        tile.lightPlace = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
        tile.sunCascade = 1;
        tile.thinStart = 0.5f;
        tile.thinEnd = 0.1f;
        tile.thinBall = ball;
        return tile;
    }

    // A small caster limited to the nearest two cascades.
    GpuCullRecord limitedCaster(float x, float y, float z) {
        GpuCullRecord record{};
        record.bounds[0] = x;
        record.bounds[1] = y;
        record.bounds[2] = z;
        record.bounds[3] = 0.3f;
        record.flags = CULL_CASTS_SHADOWS;
        record.shadowCascades = 2;
        return record;
    }

    /*
    * In a sun cascade's tile, casters limited to no further cascades thin out across a band round its ball (the camera's,
    * measured across the sun's plane): all kept within thinStart of a radius from the ball's edge, none within thinEnd,
    * the share kept falling evenly between; a caster's choice never changes. Casters free of the limit or limited to
    * further cascades, a lamp's tile, an empty band, a tile without a ball and one that thins nothing keep every one, and
    * shadowTakes leaves out what the thinning does.
    */
    void testLimitedCastersThinOut() {
        const float half = 20.0f;
        const BoundingSphere ball{Vec3{1.0f, 1.5f, -2.0f}, 18.0f};
        const ShadowTile tile = cascadeDownOver(Vec3{0.0f, 100.0f, 0.0f}, half, ball);
        const LevelOfDetailSettings settings;
        const GpuCameraCull view = packShadowCull(tile, settings);

        ShadowTile lamp = tile;
        lamp.sunCascade = -1;
        ShadowTile empty = tile;
        empty.thinStart = 0.1f;
        ShadowTile noBall = tile;
        noBall.thinBall = BoundingSphere{};
        ShadowTile thinsNothing = tile;
        thinsNothing.thinsCasters = false;
        const std::array<GpuCameraCull, 4> keepAll{packShadowCull(lamp, settings), packShadowCull(empty, settings), packShadowCull(noBall, settings),
                                                   packShadowCull(thinsNothing, settings)};

        std::mt19937 random{29};
        std::uniform_real_distribution<float> across{-half, half};
        constexpr int bins = 8;
        std::array<int, bins> kept{};
        std::array<int, bins> seen{};

        for (int index = 0; index < 40000; ++index) {
            GpuCullRecord record = limitedCaster(across(random), 0.0f, across(random));
            const float edge = 1.0f - std::hypot(record.bounds[0] - ball.centre.x, record.bounds[2] - ball.centre.z) / ball.radius;
            const bool keeps = shadowThinKeeps(record, view);
            assert(keeps == shadowThinKeeps(record, view));
            assert(shadowTakes(record, view) == keeps);

            for (const GpuCameraCull& other : keepAll) {
                assert(shadowThinKeeps(record, other));
            }

            if (edge > 0.5001f) {
                assert(keeps);
            } else if (edge < 0.0999f) {
                assert(!keeps);
            } else if (edge > 0.1f && edge < 0.5f) {
                const int bin = std::min(bins - 1, static_cast<int>((edge - 0.1f) / 0.4f * static_cast<float>(bins)));
                kept[static_cast<std::size_t>(bin)] += keeps ? 1 : 0;
                ++seen[static_cast<std::size_t>(bin)];
            }

            for (const std::int32_t cascades : {0, 3}) {
                record.shadowCascades = cascades;
                assert(shadowThinKeeps(record, view));
            }
        }

        for (int bin = 0; bin < bins; ++bin) {
            const std::size_t at = static_cast<std::size_t>(bin);
            const float share = static_cast<float>(kept[at]) / static_cast<float>(seen[at]);
            assert(seen[at] > 500);
            assert(std::abs(share - (static_cast<float>(bin) + 0.5f) / static_cast<float>(bins)) < 0.06f);
        }
    }

    /*
    * The band is the ball's, not the box's: the box stepping (moved and a little wider round the same ball) changes no
    * caster's share; a caster anywhere along a sun ray keeps its share (its shadow lands where it is across the sun's
    * plane), here a slanting sun too; and the camera's ball moving a little moves each share at most as far as the
    * band's slope allows, so walking drops casters one by one, never a batch.
    */
    void testThinningFollowsTheBall() {
        const BoundingSphere ball{Vec3{3.0f, 2.0f, -4.0f}, 100.0f};
        const GpuCameraCull view = packShadowCull(cascadeDownOver(Vec3{0.0f, 300.0f, 0.0f}, 107.0f, ball), LevelOfDetailSettings{});
        const GpuCameraCull stepped = packShadowCull(cascadeDownOver(Vec3{6.7f, 300.0f, -6.7f}, 110.0f, ball), LevelOfDetailSettings{});
        const float slope = 1.0f / ((0.5f - 0.1f) * ball.radius);

        // A slanting sun's box round the same ball.
        const Vec3 towardsSun = Vec3{0.4f, 1.0f, 0.3f}.normalized();
        ShadowTile slanting = cascadeDownOver(Vec3{0.0f, 300.0f, 0.0f}, 107.0f, ball);
        const Vec3 above = ball.centre + towardsSun * 600.0f;
        slanting.matrix = toGpuDepthRange(Mat4::orthographic(-107.0f, 107.0f, -107.0f, 107.0f, 0.0f, 1500.0f)) *
                          Mat4::lookAt(above, ball.centre, Vec3{0.0f, 0.0f, 1.0f});
        const GpuCameraCull slantingView = packShadowCull(slanting, LevelOfDetailSettings{});

        std::mt19937 random{31};
        std::uniform_real_distribution<float> across{-100.0f, 100.0f};
        std::size_t between = 0;

        for (int index = 0; index < 4000; ++index) {
            const GpuCullRecord record = limitedCaster(ball.centre.x + across(random), across(random) * 0.2f, ball.centre.z + across(random));
            const float share = shadowThinShare(record, view);
            assert(std::abs(shadowThinShare(record, stepped) - share) < 1e-4f);
            between += share > 0.0f && share < 1.0f ? 1 : 0;

            const GpuCullRecord higher = limitedCaster(record.bounds[0], record.bounds[1] + 40.0f, record.bounds[2]);
            assert(std::abs(shadowThinShare(higher, view) - share) < 1e-4f);

            const float along = across(random) * 0.5f;
            const GpuCullRecord onItsRay = limitedCaster(record.bounds[0] + towardsSun.x * along, record.bounds[1] + towardsSun.y * along,
                                                         record.bounds[2] + towardsSun.z * along);
            assert(std::abs(shadowThinShare(onItsRay, slantingView) - shadowThinShare(record, slantingView)) < 1e-3f);

            for (const float walked : {0.05f, 0.5f}) {
                GpuCameraCull moved = view;
                moved.thinBall[0] += walked;
                assert(std::abs(shadowThinShare(record, moved) - share) <= walked * slope + 1e-4f);
            }
        }

        assert(between > 1000);
    }

    /*
    * A plant's seen mesh casts into its nearest cascades and its stand-in from there on (Horizon Zero Dawn's split,
    * docs/research/FOLIAGESHADOWS.md): a caster from a first cascade (cullShadowFromBits) is taken by no nearer one; a
    * caster handing its shadow on (CULL_SHADOW_HANDED_ON) is taken whole by its last, never thinned, and names no last
    * cascade (lastCascadeBit 0), since nothing of it ends there. A lamp's tile takes the stand-in alone, as before the
    * split, so a plant casts there once.
    */
    void testCastersStartAtTheirFirstCascade() {
        const ShadowTile tile = cascadeDownOver(Vec3{0.0f, 100.0f, 0.0f}, 20.0f, BoundingSphere{Vec3{0.0f, 1.5f, 0.0f}, 18.0f});
        const LevelOfDetailSettings settings;
        std::mt19937 random{43};
        std::uniform_real_distribution<float> across{-20.0f, 20.0f};

        for (int index = 0; index < 2000; ++index) {
            GpuCullRecord standIn = limitedCaster(across(random), 0.0f, across(random));
            standIn.shadowCascades = index % 2 == 0 ? 0 : 4;
            standIn.flags |= cullShadowFromBits(2);
            assert(cullShadowFrom(standIn.flags) == 2);

            GpuCullRecord seen = limitedCaster(standIn.bounds[0], 0.0f, standIn.bounds[2]);
            seen.flags |= CULL_SHADOW_HANDED_ON;
            assert(cullShadowFrom(seen.flags) == 0 && lastCascadeBit(seen) == 0u && lastCascadeBit(standIn) == (standIn.shadowCascades == 4 ? 0b1000u : 0u));

            for (int cascade = 0; cascade <= 4; ++cascade) {
                ShadowTile each = tile;
                each.sunCascade = cascade;
                const GpuCameraCull view = packShadowCull(each, settings);
                const bool standInCasts = cascade >= 2 && (standIn.shadowCascades == 0 || cascade < standIn.shadowCascades);
                assert(standInCasts ? shadowTakes(standIn, view) || cascade == 3 : !shadowTakes(standIn, view));
                assert(shadowTakes(seen, view) == (cascade < 2));
                assert(shadowThinShare(seen, view) == 1.0f);
            }

            ShadowTile lamp = tile;
            lamp.sunCascade = -1;
            const GpuCameraCull lampView = packShadowCull(lamp, settings);
            assert(shadowTakes(standIn, lampView) && !shadowTakes(seen, lampView));
        }

        // The CPU's casters by cascade say the same (castersForCascade).
        std::vector<DrawItem> draws(2);
        draws[0].shadowCascades = 2;
        draws[0].shadowHandedOn = true;
        draws[1].shadowFromCascade = 2;
        draws[1].shadowCascades = 4;
        const std::vector<std::size_t> both{0, 1};
        assert((castersForCascade(draws, both, 1) == std::vector<std::size_t>{0}));
        assert((castersForCascade(draws, both, 2) == std::vector<std::size_t>{1}));
        assert((castersForCascade(draws, both, 4).empty()));
        assert((castersForCascade(draws, both, -1) == std::vector<std::size_t>{1}));
        assert(cullShadowFrom(packCullRecord(draws[1]).flags) == 2 && (packCullRecord(draws[0]).flags & CULL_SHADOW_HANDED_ON) != 0u);
    }

    /*
    * A cascade that thins (ShadowTile::thinsCasters) keys its cache on its ball, so it is redrawn as the camera walks
    * within a step; one that thins nothing keeps its key, so it is redrawn only when its box steps.
    */
    void testThinningCascadesKeyOnTheirBall() {
        std::mt19937 random{37};
        const CullScene scene = randomCullScene(random, 200);
        CameraCullTable table;
        const CameraCullFrame culled = table.update(frameOf(scene.draws, scene.draws.size(), 1, 0), shadowTestPlacement, [](MeshHandle) { return false; });
        const LevelOfDetailSettings settings;
        const ShadowTileMemory memory;

        ShadowTile tile = cascadeDownOver(Vec3{0.0f, 300.0f, 0.0f}, 107.0f, BoundingSphere{Vec3{0.0f, 1.5f, 0.0f}, 100.0f});
        ShadowTile walked = tile;
        walked.thinBall.centre = walked.thinBall.centre + Vec3{0.3f, 0.0f, 0.0f};
        const auto key = [&](const ShadowTile& each) {
            return shadowTileNeeds(each, settings, culled, scene.draws, {}, false, 1, memory, {}).key;
        };

        assert(key(tile) == key(tile) && key(tile) != key(walked));
        tile.thinsCasters = false;
        walked.thinsCasters = false;
        assert(key(tile) == key(walked));
    }

    /*
    * The cascades some caster is limited to last (CameraCullFrame::lastCascades): bit c for a caster limited to the
    * nearest c + 1, still or moving; a still draw rebuilt in place adds its own; one free of the limit adds none.
    */
    void testTheTableListsTheLastCascades() {
        std::mt19937 random{41};
        CullScene scene = randomCullScene(random, 400);
        constexpr std::size_t stillDraws = 300;
        std::uint32_t expected = 0;

        for (DrawItem& draw : scene.draws) {
            draw.shadowCascades = std::min(draw.shadowCascades, 2);
            expected |= draw.shadowCascades > 0 && !draw.shadowHandedOn ? 1u << (draw.shadowCascades - 1) : 0u;
        }

        assert(expected == 0b11u);
        CameraCullTable table;
        assert(table.update(frameOf(scene.draws, stillDraws, 1, 0), shadowTestPlacement, [](MeshHandle) { return false; }).lastCascades == expected);

        scene.draws[7].shadowCascades = 4;
        assert(table.update(frameOf(scene.draws, stillDraws, 2, 1, {DrawRange{7, 1}}), shadowTestPlacement, [](MeshHandle) { return false; }).lastCascades ==
               (expected | 0b1000u));

        scene.draws[350].shadowCascades = 3;
        assert(table.update(frameOf(scene.draws, stillDraws, 2, 1), shadowTestPlacement, [](MeshHandle) { return false; }).lastCascades & 0b100u);

        std::vector<DrawItem> free = scene.draws;

        for (DrawItem& draw : free) {
            draw.shadowCascades = 0;
        }

        CameraCullTable freeTable;
        assert(freeTable.update(frameOf(free, stillDraws, 1, 0), shadowTestPlacement, [](MeshHandle) { return false; }).lastCascades == 0u);
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
                const ShadowTileNeeds needs = shadowTileNeeds(tile, settings, culled, frame.draws, {}, false, frameNumber, memory, {});
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
        const ShadowTileNeeds first = shadowTileNeeds(tiles[0], settings, culled, scene.draws, {}, true, 100, memory, {});
        memory.remember(tiles[0], first.key);
        assert(shadowTileNeeds(tiles[0], settings, culled, scene.draws, {}, true, 101, memory, {}).redraw);
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

    /*
    * Which tiles keep their movers apart (keepsMoversApart): the far map always; a sun cascade once a moving draw is in
    * it, and from then on while it has its still copy; never a swaying cascade (redrawn every frame anyway) or a lamp's
    * tile. Kept apart, a mover moving leaves the cascade's still casters cached, where the whole tile was redrawn.
    */
    void testCascadesKeepTheirMoversApart() {
        std::mt19937 random{83};
        CullScene scene = randomCullScene(random, 300);
        constexpr std::size_t stillDraws = 250;

        for (DrawItem& draw : scene.draws) {
            draw.previousModel = draw.model;
        }

        // A cascade's box straight down over a mover that casts.
        const auto boxOver = [](const Vec3& centre) {
            ShadowTile tile;
            const Vec3 place = centre + Vec3{0.0f, 500.0f, 0.0f};
            tile.matrix = toGpuDepthRange(Mat4::orthographic(-100.0f, 100.0f, -100.0f, 100.0f, 0.0f, 1500.0f)) *
                          Mat4::lookAt(place, centre, Vec3{0.0f, 0.0f, 1.0f});
            tile.lightPlace = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
            tile.size = 1024;
            tile.sunCascade = 2;
            return tile;
        };
        const LevelOfDetailSettings settings;
        std::size_t mover = scene.draws.size();

        for (std::size_t draw = stillDraws; draw < scene.draws.size() && mover == scene.draws.size(); ++draw) {
            const DrawItem& item = scene.draws[draw];

            if (item.bounds.radius > 0.0f && shadowTakes(packCullRecord(item), packShadowCull(boxOver(item.bounds.centre), settings))) {
                mover = draw;
            }
        }

        assert(mover < scene.draws.size());
        // Moving already, as the drifting rocks always are.
        scene.draws[mover].previousModel = Mat4::translation(-1.0f, 0.0f, 0.0f) * scene.draws[mover].model;
        CameraCullTable table;
        const CameraCullFrame culled = table.update(frameOf(scene.draws, stillDraws, 1, 0), shadowTestPlacement, [](MeshHandle) { return false; });

        const ShadowTile cascade = boxOver(scene.draws[mover].bounds.centre);
        const ShadowTile empty = boxOver(Vec3{50000.0f, 0.0f, 50000.0f});
        ShadowTile lamp = cascade;
        lamp.sunCascade = -1;
        ShadowTile far = empty;
        far.separateMovers = true;

        assert(keepsMoversApart(cascade, culled, false, false));
        assert(!keepsMoversApart(cascade, culled, true, false) && !keepsMoversApart(cascade, culled, true, true));
        assert(!keepsMoversApart(empty, culled, false, false) && keepsMoversApart(empty, culled, false, true));
        assert(!keepsMoversApart(lamp, culled, false, false) && !keepsMoversApart(lamp, culled, false, true));
        assert(keepsMoversApart(far, culled, false, false) && keepsMoversApart(far, culled, true, false));

        // The mover moves a metre: the whole cascade redrawn while its movers are not apart, its still casters cached once
        // they are.
        ShadowTile apart = cascade;
        apart.separateMovers = true;
        ShadowTileMemory memory;
        memory.remember(cascade, shadowTileNeeds(cascade, settings, culled, scene.draws, {}, false, 1, memory, {}).key);
        ShadowTileMemory apartMemory;
        apartMemory.remember(apart, shadowTileNeeds(apart, settings, culled, scene.draws, {}, false, 1, apartMemory, {}).key);

        DrawItem& moving = scene.draws[mover];
        moving.previousModel = moving.model;
        moving.model = Mat4::translation(1.0f, 0.0f, 0.0f) * moving.model;
        moving.bounds.centre = moving.bounds.centre + Vec3{1.0f, 0.0f, 0.0f};
        const CameraCullFrame moved = table.update(frameOf(scene.draws, stillDraws, 2, 1), shadowTestPlacement, [](MeshHandle) { return false; });

        assert(shadowTileNeeds(cascade, settings, moved, scene.draws, {}, false, 2, memory, {}).redraw);
        const ShadowTileNeeds apartNeeds = shadowTileNeeds(apart, settings, moved, scene.draws, {}, false, 2, apartMemory, {});
        assert(!apartNeeds.redraw && apartNeeds.movers.moving.size() == 1 && apartNeeds.movers.moving.front() == mover);
    }

    /*
    * The placed plants' changes (render/gpu/placed_drawing.h): with nothing else changed, a tile is redrawn exactly when
    * a cell made or dropped is one of its casters by its sphere (shadowTakes); a new table redraws every tile.
    */
    void testPlantedChangesRedrawTheirTiles() {
        std::mt19937 random{79};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        CullScene scene = randomCullScene(random, 400);

        for (DrawItem& draw : scene.draws) {
            draw.previousModel.reset();
        }

        std::vector<ShadowTile> tiles;

        for (int index = 0; index < 40; ++index) {
            ShadowTile tile = randomTile(random);
            tile.x = static_cast<std::uint32_t>(index) * 4096;
            tiles.push_back(tile);
        }

        const LevelOfDetailSettings settings;
        CameraCullTable table;
        ShadowTileMemory memory;
        const CameraCullFrame first = table.update(frameOf(scene.draws, scene.draws.size(), 1, 0), shadowTestPlacement, [](MeshHandle) { return false; });

        for (const ShadowTile& tile : tiles) {
            memory.remember(tile, shadowTileNeeds(tile, settings, first, scene.draws, {}, false, 1, memory, {}).key);
        }

        const CameraCullFrame culled = table.update(frameOf(scene.draws, scene.draws.size(), 2, 1), shadowTestPlacement, [](MeshHandle) { return false; });
        PlantedShadowChanges planted;

        for (int cell = 0; cell < 6; ++cell) {
            GpuCullRecord record{};
            record.bounds[0] = (unit(random) - 0.5f) * 3000.0f;
            record.bounds[1] = unit(random) * 60.0f;
            record.bounds[2] = (unit(random) - 0.5f) * 3000.0f;
            record.bounds[3] = 40.0f + unit(random) * 60.0f;
            record.flags = CULL_CASTS_SHADOWS;
            planted.cells.push_back(record);
        }

        std::size_t touched = 0;

        for (const ShadowTile& tile : tiles) {
            const ShadowTileNeeds needs = shadowTileNeeds(tile, settings, culled, scene.draws, {}, false, 2, memory, planted);
            const bool takes = std::any_of(planted.cells.begin(), planted.cells.end(), [&](const GpuCullRecord& record) {
                return shadowTakes(record, needs.view);
            });
            assert(needs.redraw == takes);
            touched += takes ? 1 : 0;
            assert(!shadowTileNeeds(tile, settings, culled, scene.draws, {}, false, 2, memory, {}).redraw);
        }

        assert(touched > 0 && touched < tiles.size());

        planted.all = true;

        for (const ShadowTile& tile : tiles) {
            assert(shadowTileNeeds(tile, settings, culled, scene.draws, {}, false, 2, memory, planted).redraw);
        }
    }
}

/*
* Shadow tiles as views of the GPU culling (render/gpu/shadow_culling.h): their lists equal the CPU's casters, and the
* cache's announced-change rule redraws every tile the casters' fingerprint would.
*/
void testShadowCulling() {
    testShadowListsMatchTheCpu();
    testLimitedCastersThinOut();
    testThinningFollowsTheBall();
    testThinningCascadesKeyOnTheirBall();
    testCastersStartAtTheirFirstCascade();
    testTheTableListsTheLastCascades();
    testShadowCacheCoversTheFingerprint();
    testShadowEdges();
    testCascadesKeepTheirMoversApart();
    testPlantedChangesRedrawTheirTiles();
}
