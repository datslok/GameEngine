#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/transform.h"
#include "scene/mesh.h"
#include "scene/deforming_mesh.h"
#include "scene/frame_description.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"
#include "scene/still_draws.h"
#include "scene/world_cells.h"

#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
    // Still entities' meshes are numbered from here, so a frame built from scratch can be sorted as the kept one is.
    constexpr std::uint32_t firstStillMesh = 100;

    Camera testCamera() {
        return Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }

    Transform placedAt(float x, float y, float z) {
        Transform transform;
        transform.position = Vec3{x, y, z};
        return transform;
    }

    // An entity with parts meshes, numbered from mesh, still or moving.
    Entity addModel(World& world, std::uint32_t mesh, int parts, bool still, const Transform& transform) {
        ModelRenderer renderer;
        renderer.still = still;

        for (int part = 0; part < parts; ++part) {
            Material material;
            material.roughness = 0.1f * static_cast<float>(part + 1);
            renderer.parts.push_back(RenderPart{MeshHandle{mesh + static_cast<std::uint32_t>(part)}, material, Mat4::translation(0.0f, static_cast<float>(part), 0.0f)});
        }

        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, renderer);
        return entity;
    }

    bool isStillMesh(const DrawItem& draw) {
        return draw.mesh.index >= firstStillMesh;
    }

    // Two draws alike in everything a frame built from scratch fills in, and with motion, their last-frame matrices too
    // (none counts as not moving).
    bool sameDraw(const DrawItem& a, const DrawItem& b, bool withMotion = true) {
        return a.mesh == b.mesh && a.model == b.model && a.material.roughness == b.material.roughness && a.material.colour.r == b.material.colour.r &&
               a.castsShadows == b.castsShadows && a.isStatic == b.isStatic && a.shadowOnly == b.shadowOnly && a.shadowCascades == b.shadowCascades &&
               a.hidden == b.hidden && (!withMotion || a.previousModel.value_or(a.model) == b.previousModel.value_or(b.model));
    }

    // Cells without spare room, a slot each at the least: kept draws land where the tests before cells expect them.
    CellSettings tightCells() {
        CellSettings settings;
        settings.spareShare = 0.0f;
        settings.fewestSlots = 1;
        return settings;
    }

    StillDraws tightStillDraws() {
        StillDraws still;
        still.setCells(tightCells());
        return still;
    }

    // A slot no draw fills: nothing to draw, hidden, no shadow, bounds unknown.
    bool isEmptySlot(const DrawItem& draw) {
        return !draw.mesh.isValid() && draw.hidden && !draw.castsShadows && draw.bounds.radius < 0.0f;
    }

    // Draws in a fixed order (mesh, then where they stand), so lists laid out differently can be compared.
    void sortDraws(std::vector<DrawItem>& draws) {
        std::sort(draws.begin(), draws.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.mesh.index != b.mesh.index) {
                return a.mesh.index < b.mesh.index;
            }

            for (int row = 0; row < 3; ++row) {
                if (a.model.values[row][3] != b.model.values[row][3]) {
                    return a.model.values[row][3] < b.model.values[row][3];
                }
            }

            return false;
        });
    }

    /*
    * The frame built from the kept draws against one built from scratch: its still part, empty slots left out, holds
    * the scratch frame's still draws (laid out by cell, so compared in a fixed order), every other slot is empty, and
    * the moving draws after them are the scratch frame's in its order.
    */
    void assertMatchesScratch(World& world, StillDraws& still, MotionHistory& history, MotionHistory& scratchHistory, float alpha = 0.0f) {
        FrameDescription kept = buildFrame(world, testCamera(), alpha, &history, serialJobs(), 0, &still);
        FrameDescription scratch = buildFrame(world, testCamera(), alpha, &scratchHistory);
        std::stable_partition(scratch.draws.begin(), scratch.draws.end(), isStillMesh);
        clearRenderChanged(world);

        const std::size_t scratchStill = static_cast<std::size_t>(std::count_if(scratch.draws.begin(), scratch.draws.end(), isStillMesh));
        std::vector<DrawItem> keptStill;

        // A still draw never moved since last frame as far as motion vectors go: an announced move is a jump.
        for (std::size_t draw = 0; draw < kept.stillDraws; ++draw) {
            const DrawItem& item = kept.draws[draw];
            assert(isEmptySlot(item) || isStillMesh(item));

            if (!isEmptySlot(item)) {
                assert(item.previousModel && *item.previousModel == item.model);
                keptStill.push_back(item);
            }
        }

        std::vector<DrawItem> scratchStillDraws(scratch.draws.begin(), scratch.draws.begin() + static_cast<std::ptrdiff_t>(scratchStill));
        sortDraws(keptStill);
        sortDraws(scratchStillDraws);
        assert(keptStill.size() == scratchStill);

        for (std::size_t draw = 0; draw < scratchStill; ++draw) {
            assert(sameDraw(keptStill[draw], scratchStillDraws[draw], false));
        }

        assert(kept.draws.size() - kept.stillDraws == scratch.draws.size() - scratchStill);

        for (std::size_t draw = kept.stillDraws; draw < kept.draws.size(); ++draw) {
            assert(sameDraw(kept.draws[draw], scratch.draws[scratchStill + draw - kept.stillDraws]));
        }

        still.giveBack(std::move(kept.draws));
    }

    // How many of the still draws a frame says it built.
    std::size_t builtCount(const FrameDescription& frame) {
        std::size_t count = 0;

        for (const DrawRange& range : frame.builtStillDraws) {
            assert(range.first + range.count <= frame.stillDraws);
            count += range.count;
        }

        return count;
    }

    /*
    * Still draws are kept from frame to frame: the first frame builds them all, the next builds none (a value written
    * into a kept draw is still there), and moving draws are built every frame after them. A still draw's last-frame
    * matrix is its own.
    */
    void testStillDrawsAreKept() {
        World world;
        addModel(world, 1, 1, false, placedAt(1.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh, 2, true, placedAt(2.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh + 10, 1, true, placedAt(3.0f, 0.0f, 0.0f));
        StillDraws still = tightStillDraws();
        MotionHistory history;

        FrameDescription first = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(first.draws.size() == 4 && first.stillDraws == 3 && builtCount(first) == 3);
        assert(isStillMesh(first.draws[0]) && isStillMesh(first.draws[2]) && !isStillMesh(first.draws[3]));
        assert(first.draws[0].previousModel && *first.draws[0].previousModel == first.draws[0].model);
        first.draws[1].bounds.radius = 123.0f;
        still.giveBack(std::move(first.draws));

        FrameDescription second = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(second.draws.size() == 4 && second.stillDraws == 3 && builtCount(second) == 0);
        assert(second.draws[1].bounds.radius == 123.0f);
        assert(second.draws[3].mesh == MeshHandle{1});

        // Each frame a new version, naming the one before, so the renderer knows whether it saw that one.
        assert(second.stillVersion != first.stillVersion && second.stillPreviousVersion == first.stillVersion);
        still.giveBack(std::move(second.draws));

        // Room kept for the terrain's draws as without kept draws.
        FrameDescription third = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 40, &still);
        assert(third.draws.capacity() >= third.draws.size() + 40);
    }

    /*
    * A change announced with markRenderChanged is built that frame, in place: only that entity's draws. Moving entities
    * are built every frame whatever they do.
    */
    void testAnnouncedChangesAreBuilt() {
        World world;
        const Entity mover = addModel(world, 1, 1, false, placedAt(1.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh, 2, true, placedAt(2.0f, 0.0f, 0.0f));
        const Entity changing = addModel(world, firstStillMesh + 10, 2, true, placedAt(3.0f, 0.0f, 0.0f));
        StillDraws still = tightStillDraws();
        MotionHistory history;
        MotionHistory scratchHistory;
        assertMatchesScratch(world, still, history, scratchHistory);

        world.get<Transform>(mover).position.x = 9.0f;
        world.get<Transform>(changing).position.y = 4.0f;
        world.get<ModelRenderer>(changing).parts[1].material.roughness = 0.9f;
        markRenderChanged(world, changing);
        markRenderChanged(world, changing); // twice is once

        FrameDescription frame = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        buildFrame(world, testCamera(), 0.0f, &scratchHistory); // the same frames for the scratch history
        assert(frame.builtStillDraws.size() == 1 && frame.builtStillDraws[0].first == 2 && frame.builtStillDraws[0].count == 2);
        assert(frame.draws[3].material.roughness == 0.9f && frame.draws[2].model.values[1][3] == 4.0f);
        assert(frame.draws[4].model.values[0][3] == 9.0f);
        clearRenderChanged(world);
        assert(!world.has<RenderChanged>(changing));
        still.giveBack(std::move(frame.draws));

        // Moving entities and announced changes match a frame from scratch.
        world.get<Transform>(mover).position.x = 11.0f;
        world.get<Transform>(changing).position.y = 5.0f;
        markRenderChanged(world, changing);
        assertMatchesScratch(world, still, history, scratchHistory);
        assertMatchesScratch(world, still, history, scratchHistory);
    }

    /*
    * What the kept draws notice by themselves: a still entity added or destroyed, hidden or shown, no longer still,
    * given more parts, or moving a deforming mesh (always built: its shape changes); and a frame not given back, or
    * kept draws let go (the assets' levels changed). Each time, the frame matches one from scratch.
    */
    void testTheKeptDrawsFollowTheWorld() {
        World world;
        addModel(world, 1, 1, false, placedAt(1.0f, 0.0f, 0.0f));
        const Entity a = addModel(world, firstStillMesh, 2, true, placedAt(2.0f, 0.0f, 0.0f));
        const Entity b = addModel(world, firstStillMesh + 10, 1, true, placedAt(3.0f, 0.0f, 0.0f));
        const Entity c = addModel(world, firstStillMesh + 20, 1, true, placedAt(4.0f, 0.0f, 0.0f));
        StillDraws still = tightStillDraws();
        MotionHistory history;
        MotionHistory scratchHistory;
        assertMatchesScratch(world, still, history, scratchHistory);

        addModel(world, firstStillMesh + 30, 3, true, placedAt(5.0f, 0.0f, 0.0f));
        assertMatchesScratch(world, still, history, scratchHistory);

        world.destroy(b);
        assertMatchesScratch(world, still, history, scratchHistory);

        world.get<ModelRenderer>(a).visible = false;
        assertMatchesScratch(world, still, history, scratchHistory);
        world.get<ModelRenderer>(a).visible = true;
        assertMatchesScratch(world, still, history, scratchHistory);

        // No longer still: it is built every frame, among the moving draws (its meshes renumbered so the sort agrees).
        ModelRenderer& unstill = world.get<ModelRenderer>(c);
        unstill.still = false;
        unstill.parts[0].mesh = MeshHandle{7};
        assertMatchesScratch(world, still, history, scratchHistory);

        world.get<ModelRenderer>(a).parts.push_back(RenderPart{MeshHandle{firstStillMesh + 5}, Material{}, Mat4::identity()});
        assertMatchesScratch(world, still, history, scratchHistory);

        // A deforming mesh is never kept, still or not.
        const Entity deforming = addModel(world, 8, 1, true, placedAt(6.0f, 0.0f, 0.0f));
        world.add(deforming, DeformingMesh{MeshHandle{8}, {}, {}});
        FrameDescription withShape = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(std::none_of(withShape.draws.begin(), withShape.draws.begin() + static_cast<std::ptrdiff_t>(withShape.stillDraws),
                            [](const DrawItem& draw) { return draw.mesh == MeshHandle{8}; }));
        still.giveBack(std::move(withShape.draws));
        assertMatchesScratch(world, still, history, scratchHistory);

        // A frame not given back: the next lays them all out again (packed afresh, so perhaps in fewer slots).
        std::size_t stillCount = 0;
        {
            const FrameDescription lost = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
            stillCount = lost.stillDraws;
        }
        FrameDescription rebuilt = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(rebuilt.stillDraws <= stillCount && builtCount(rebuilt) == rebuilt.stillDraws);
        still.giveBack(std::move(rebuilt.draws));

        still.invalidate();
        FrameDescription afterInvalidate = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(builtCount(afterInvalidate) == afterInvalidate.stillDraws);
        still.giveBack(std::move(afterInvalidate.draws));
        assertMatchesScratch(world, still, history, scratchHistory);
    }

    // Built on many cores, the kept frame is the same as on one.
    void testKeptDrawsOnManyCores() {
        World world;

        for (int index = 0; index < 600; ++index) {
            const bool isStill = index % 3 != 0;
            addModel(world, isStill ? firstStillMesh + static_cast<std::uint32_t>(index) : 1, 2, isStill, placedAt(static_cast<float>(index), 0.0f, 0.0f));
        }

        StillDraws alone;
        StillDraws together;
        MotionHistory aloneHistory;
        MotionHistory togetherHistory;
        const FrameDescription one = buildFrame(world, testCamera(), 0.0f, &aloneHistory, serialJobs(), 0, &alone);
        const FrameDescription many = buildFrame(world, testCamera(), 0.0f, &togetherHistory, testJobs(), 0, &together);
        assert(one.draws.size() == many.draws.size() && one.stillDraws == many.stillDraws);

        for (std::size_t draw = 0; draw < one.draws.size(); ++draw) {
            assert(sameDraw(one.draws[draw], many.draws[draw]));
        }
    }

    /*
    * The debug builds' check: a still entity that moved without markRenderChanged throws, naming the mistake; once it
    * is announced and built, the check passes.
    */
    void testAnUnannouncedMoveIsCaught() {
        World world;
        const Entity entity = addModel(world, firstStillMesh, 1, true, placedAt(2.0f, 0.0f, 0.0f));
        StillDraws still = tightStillDraws();
        MotionHistory history;
        FrameDescription frame = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        still.giveBack(std::move(frame.draws));
        still.verify(world, 100);

        world.get<Transform>(entity).position.x = 3.0f;
        bool caught = false;

        try {
            still.verify(world, 100);
        } catch (const std::logic_error&) {
            caught = true;
        }

        assert(caught);
        markRenderChanged(world, entity);
        frame = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        clearRenderChanged(world);
        still.giveBack(std::move(frame.draws));
        still.verify(world, 100);
    }
}

namespace {
    // The assets' levels version moves whenever levels or options are named, so the engine lets kept draws go.
    void testNamingLevelsChangesTheVersion() {
        AssetManager assets;
        const MeshHandle base = assets.addMesh(Mesh::grid(2, 2, 1.0f, 1.0f));
        const MeshHandle coarse = assets.addMesh(Mesh::grid(1, 1, 1.0f, 1.0f));
        const std::uint64_t start = assets.getLevelsVersion();

        assets.setMeshLevels(base, {MeshLevel{coarse, 0.1f}});
        const std::uint64_t named = assets.getLevelsVersion();
        assert(named != start);

        assets.setMeshLevelOptions(base, MeshLevelOptions{});
        assert(assets.getLevelsVersion() != named);
    }
}

namespace {
    // The ranges a frame says it built, as (first, count) pairs.
    std::vector<std::pair<std::size_t, std::size_t>> builtRanges(const FrameDescription& frame) {
        std::vector<std::pair<std::size_t, std::size_t>> ranges;

        for (const DrawRange& range : frame.builtStillDraws) {
            ranges.emplace_back(range.first, range.count);
        }

        return ranges;
    }

    using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

    /*
    * Still draws are laid out by cell (scene/world_cells.h), cells by column then row, each a block with spare room
    * (64 slots at the least by default); a block's slots past its entities' draws are empty. Moving draws follow.
    */
    void testDrawsAreLaidOutByCell() {
        World world;
        addModel(world, firstStillMesh, 2, true, placedAt(10.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh + 10, 1, true, placedAt(300.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh + 20, 1, true, placedAt(20.0f, 0.0f, 5.0f));
        addModel(world, 1, 1, false, placedAt(1.0f, 0.0f, 0.0f));
        StillDraws still;
        MotionHistory history;

        FrameDescription frame = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(frame.stillDraws == 128 && frame.draws.size() == 129 && still.getCellCount() == 2 && frame.stillCells == 2);
        assert(still.getFilledCount() == 4 && frame.stillEmptySlots == 124);
        assert((builtRanges(frame) == Ranges{{0, 128}}));
        assert(frame.draws[0].mesh == MeshHandle{firstStillMesh} && frame.draws[1].mesh == MeshHandle{firstStillMesh + 1});
        assert(frame.draws[2].mesh == MeshHandle{firstStillMesh + 20});
        assert(frame.draws[64].mesh == MeshHandle{firstStillMesh + 10});
        assert(frame.draws[128].mesh == MeshHandle{1});

        for (const std::size_t empty : {std::size_t{3}, std::size_t{63}, std::size_t{65}, std::size_t{127}}) {
            assert(isEmptySlot(frame.draws[empty]));
        }

        still.giveBack(std::move(frame.draws));
    }

    /*
    * Adding, destroying or moving a still entity across cells rebuilds the cells it leaves and joins, nothing else; an
    * announced change within its cell is built in place. Each time, the frame matches one from scratch.
    */
    void testAChangeRebuildsOnlyItsCells() {
        World world;
        const Entity a = addModel(world, firstStillMesh, 2, true, placedAt(10.0f, 0.0f, 0.0f));
        const Entity b = addModel(world, firstStillMesh + 10, 1, true, placedAt(300.0f, 0.0f, 0.0f));
        const Entity a2 = addModel(world, firstStillMesh + 20, 1, true, placedAt(20.0f, 0.0f, 5.0f));
        addModel(world, 1, 1, false, placedAt(1.0f, 0.0f, 0.0f));
        StillDraws still;
        MotionHistory history;
        MotionHistory scratchHistory;
        assertMatchesScratch(world, still, history, scratchHistory);

        const auto nextFrame = [&]() {
            FrameDescription frame = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
            buildFrame(world, testCamera(), 0.0f, &scratchHistory);
            clearRenderChanged(world);
            return frame;
        };

        // Added to the cell east: that cell's block alone.
        const Entity c = addModel(world, firstStillMesh + 30, 1, true, placedAt(310.0f, 0.0f, 0.0f));
        FrameDescription added = nextFrame();
        assert((builtRanges(added) == Ranges{{64, 64}}) && added.stillDraws == 128);
        assert(added.draws[65].mesh == MeshHandle{firstStillMesh + 30});
        still.giveBack(std::move(added.draws));
        assertMatchesScratch(world, still, history, scratchHistory);

        // Destroyed: its cell's block, the gap closed within it.
        world.destroy(a2);
        FrameDescription destroyed = nextFrame();
        assert((builtRanges(destroyed) == Ranges{{0, 64}}));
        assert(destroyed.draws[1].mesh == MeshHandle{firstStillMesh + 1} && isEmptySlot(destroyed.draws[2]));
        assert(still.getFilledCount() == 4 && destroyed.stillEmptySlots == 124);
        still.giveBack(std::move(destroyed.draws));
        assertMatchesScratch(world, still, history, scratchHistory);

        // Moved across cells, announced: both cells' blocks (touching, so one range).
        world.get<Transform>(b).position = Vec3{20.0f, 0.0f, 0.0f};
        markRenderChanged(world, b);
        FrameDescription moved = nextFrame();
        assert((builtRanges(moved) == Ranges{{0, 128}}));
        assert(moved.draws[2].mesh == MeshHandle{firstStillMesh + 10} && moved.draws[64].mesh == MeshHandle{firstStillMesh + 30});
        assert(moved.draws[2].model.values[0][3] == 20.0f && isEmptySlot(moved.draws[65]));
        still.giveBack(std::move(moved.draws));
        assertMatchesScratch(world, still, history, scratchHistory);

        // Changed within its cell: in place, its own draws.
        world.get<ModelRenderer>(c).parts[0].material.roughness = 0.7f;
        markRenderChanged(world, c);
        FrameDescription inPlace = nextFrame();
        assert((builtRanges(inPlace) == Ranges{{64, 1}}) && inPlace.draws[64].material.roughness == 0.7f);
        still.giveBack(std::move(inPlace.draws));

        // Given another part: its cell's block.
        world.get<ModelRenderer>(a).parts.push_back(RenderPart{MeshHandle{firstStillMesh + 5}, Material{}, Mat4::identity()});
        FrameDescription reshaped = nextFrame();
        assert((builtRanges(reshaped) == Ranges{{0, 64}}));
        still.giveBack(std::move(reshaped.draws));
        assertMatchesScratch(world, still, history, scratchHistory);
        assert(still.getCellCount() == 2);
    }

    /*
    * A cell outgrowing its block moves to the first free run long enough (else the end), its old block emptied and
    * reported built; a run given back is used by the next cell that fits it.
    */
    void testACellOutgrowingItsBlockMoves() {
        CellSettings small;
        small.spareShare = 0.0f;
        small.fewestSlots = 2;
        World world;
        addModel(world, firstStillMesh, 2, true, placedAt(1.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh + 10, 1, true, placedAt(200.0f, 0.0f, 0.0f));
        StillDraws still;
        still.setCells(small);
        MotionHistory history;
        MotionHistory scratchHistory;
        assertMatchesScratch(world, still, history, scratchHistory);

        addModel(world, firstStillMesh + 20, 1, true, placedAt(2.0f, 0.0f, 0.0f));
        FrameDescription grown = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        buildFrame(world, testCamera(), 0.0f, &scratchHistory);
        assert((builtRanges(grown) == Ranges{{0, 2}, {4, 3}}) && grown.stillDraws == 7);
        assert(isEmptySlot(grown.draws[0]) && isEmptySlot(grown.draws[1]) && grown.draws[2].mesh == MeshHandle{firstStillMesh + 10});
        assert(grown.draws[4].mesh == MeshHandle{firstStillMesh} && grown.draws[6].mesh == MeshHandle{firstStillMesh + 20});
        still.giveBack(std::move(grown.draws));
        assertMatchesScratch(world, still, history, scratchHistory);

        // A new cell of two draws fits the run given back.
        addModel(world, firstStillMesh + 30, 2, true, placedAt(500.0f, 0.0f, 0.0f));
        FrameDescription reused = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        buildFrame(world, testCamera(), 0.0f, &scratchHistory);
        assert((builtRanges(reused) == Ranges{{0, 2}}) && reused.stillDraws == 7);
        assert(reused.draws[0].mesh == MeshHandle{firstStillMesh + 30});
        still.giveBack(std::move(reused.draws));
        assertMatchesScratch(world, still, history, scratchHistory);
        assert(still.getCellCount() == 3);
    }

    // Other cell settings lay every still draw out again; the same settings again change nothing.
    void testNewCellSettingsLayEverythingOut() {
        World world;
        addModel(world, firstStillMesh, 1, true, placedAt(1.0f, 0.0f, 0.0f));
        addModel(world, firstStillMesh + 10, 1, true, placedAt(100.0f, 0.0f, 0.0f));
        StillDraws still;
        MotionHistory history;
        FrameDescription first = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(first.stillDraws == 64 && still.getCellCount() == 1);
        still.giveBack(std::move(first.draws));

        still.setCells(CellSettings{});
        FrameDescription same = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(same.builtStillDraws.empty());
        still.giveBack(std::move(same.draws));

        CellSettings smaller;
        smaller.size = 64.0f;
        still.setCells(smaller);
        FrameDescription relaid = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(relaid.stillDraws == 128 && builtCount(relaid) == 128 && still.getCellCount() == 2);
        still.giveBack(std::move(relaid.draws));
    }
}

void testStillDraws() {
    testDrawsAreLaidOutByCell();
    testAChangeRebuildsOnlyItsCells();
    testACellOutgrowingItsBlockMoves();
    testNewCellSettingsLayEverythingOut();
    testNamingLevelsChangesTheVersion();
    testStillDrawsAreKept();
    testAnnouncedChangesAreBuilt();
    testTheKeptDrawsFollowTheWorld();
    testKeptDrawsOnManyCores();
    testAnUnannouncedMoveIsCaught();
}
