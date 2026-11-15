#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/transform.h"
#include "scene/mesh.h"
#include "scene/deforming_mesh.h"
#include "scene/frame_description.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"
#include "scene/still_draws.h"

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

    /*
    * The frame built from the kept draws against one built from scratch with its still draws moved to the front (in their
    * order), as the kept frame lays them out: the same draws, the still count right.
    */
    void assertMatchesScratch(World& world, StillDraws& still, MotionHistory& history, MotionHistory& scratchHistory, float alpha = 0.0f) {
        FrameDescription kept = buildFrame(world, testCamera(), alpha, &history, serialJobs(), 0, &still);
        FrameDescription scratch = buildFrame(world, testCamera(), alpha, &scratchHistory);
        std::stable_partition(scratch.draws.begin(), scratch.draws.end(), isStillMesh);
        clearRenderChanged(world);

        assert(kept.draws.size() == scratch.draws.size());
        assert(kept.stillDraws == static_cast<std::size_t>(std::count_if(scratch.draws.begin(), scratch.draws.end(), isStillMesh)));

        // A still draw never moved since last frame as far as motion vectors go: an announced move is a jump.
        for (std::size_t draw = 0; draw < kept.draws.size(); ++draw) {
            const bool isStill = draw < kept.stillDraws;
            assert(sameDraw(kept.draws[draw], scratch.draws[draw], !isStill));
            assert(!isStill || (kept.draws[draw].previousModel && *kept.draws[draw].previousModel == kept.draws[draw].model));
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
        StillDraws still;
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
        StillDraws still;
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
        StillDraws still;
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

        // A frame not given back: the next builds them all again.
        std::size_t stillCount = 0;
        {
            const FrameDescription lost = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
            stillCount = lost.stillDraws;
        }
        FrameDescription rebuilt = buildFrame(world, testCamera(), 0.0f, &history, serialJobs(), 0, &still);
        assert(rebuilt.stillDraws == stillCount && builtCount(rebuilt) == stillCount);
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
        StillDraws still;
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

void testStillDraws() {
    testNamingLevelsChangesTheVersion();
    testStillDrawsAreKept();
    testAnnouncedChangesAreBuilt();
    testTheKeptDrawsFollowTheWorld();
    testKeptDrawsOnManyCores();
    testAnUnannouncedMoveIsCaught();
}
