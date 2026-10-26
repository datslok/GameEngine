#include "engine/engine_settings.h"
#include "engine/gpu_cloth.h"
#include "games/clothlab/cloth_lab_game.h"
#include "test_levels.h"

#include <cassert>
#include <utility>
#include <vector>

namespace {
    Entity addSheet(World& world, bool onGpu, std::uint32_t mesh) {
        const Entity sheet = world.create();
        Cloth cloth = makeCloth(Mesh::grid(2, 2, 1.0f, 1.0f), Mat4::identity(), ClothSettings{});
        addClothPin(cloth, 6);
        if (onGpu) {
            cloth.onGpu = true;
            cloth.gpuTicks.push_back(clothTickCommands(cloth, 1.0f / 120.0f));
            cloth.gpuTicks.push_back(clothTickCommands(cloth, 1.0f / 120.0f));
        }
        world.add(sheet, DeformingMesh{MeshHandle{mesh}, cloth.particles.positions, cloth.particles.positions});
        world.add(sheet, std::move(cloth));
        return sheet;
    }

    // On by default where there is a GPU; a game is told so by the engine, and is on the CPU until then.
    void testDefaults() {
        assert(EngineSettings{}.clothOnGpu);
        ClothLabGame game;
        assert(game.getClothSolver() == ClothSolver::Cpu);
        game.useClothSolver(ClothSolver::Gpu);
        assert(game.getClothSolver() == ClothSolver::Gpu);
    }

    // Each cloth on the GPU hands over its queued ticks (and keeps none) with its mesh; cloths on the CPU hand nothing.
    void testTakeClothWork() {
        World world;
        const Entity gpuSheet = addSheet(world, true, 4);
        addSheet(world, false, 5);

        const std::vector<ClothWork> work = takeClothWork(world);
        assert(work.size() == 1);
        assert(work[0].key == clothKey(gpuSheet));
        assert(work[0].cloth == &world.get<Cloth>(gpuSheet));
        assert(work[0].mesh.index == 4);
        assert(work[0].ticks.size() == 2);
        assert(world.get<Cloth>(gpuSheet).gpuTicks.empty());

        // A frame with no ticks still hands the cloth over: it is drawn between its last two ticks.
        const std::vector<ClothWork> idle = takeClothWork(world);
        assert(idle.size() == 1 && idle[0].ticks.empty());
    }

    // Keys tell entities apart by generation too, so a recycled slot is a new cloth.
    void testKeys() {
        World world;
        const Entity first = world.create();
        world.destroy(first);
        const Entity second = world.create();
        assert(first.index == second.index);
        assert(clothKey(first) != clothKey(second));
    }

    // The GPU's points, read back late, become the cloth's and its mesh's (both of its mesh's ticks: it is not blended).
    void testApplyReadbacks() {
        World world;
        const Entity sheet = addSheet(world, true, 0);
        const std::size_t points = world.get<Cloth>(sheet).particles.size();
        ClothReadback readback{clothKey(sheet), std::vector<Vec3>(points, Vec3{1.0f, 2.0f, 3.0f}), std::vector<Vec3>(points, Vec3{0.0f, -1.0f, 0.0f})};
        const ClothReadback stranger{clothKey(sheet) + 1, {}, {}};
        const ClothReadback wrongSize{clothKey(sheet), std::vector<Vec3>(points + 1), std::vector<Vec3>(points + 1)};
        applyClothReadbacks(world, std::vector<ClothReadback>{wrongSize, std::move(readback), stranger});

        const Cloth& cloth = world.get<Cloth>(sheet);
        assert(cloth.particles.positions[0].y == 2.0f && cloth.particles.velocities[3].y == -1.0f);
        const DeformingMesh& mesh = world.get<DeformingMesh>(sheet);
        assert(mesh.positions[2].z == 3.0f && mesh.previousPositions[2].z == 3.0f);

        // A cloth back on the CPU keeps its own points: a readback from frames still in flight comes too late for it.
        world.get<Cloth>(sheet).onGpu = false;
        const ClothReadback late{clothKey(sheet), std::vector<Vec3>(points, Vec3{9.0f, 9.0f, 9.0f}), std::vector<Vec3>(points)};
        applyClothReadbacks(world, std::vector<ClothReadback>{late});
        assert(world.get<Cloth>(sheet).particles.positions[0].y == 2.0f);
    }

    /*
    * The points the CPU holds are a few frames old, so a GPU cloth's bounds are grown by how far its fastest point can
    * have gone since, plus the cloth's thickness: never smaller than the box round its points.
    */
    void testLateBounds() {
        ClothParticles particles = makeClothParticles(Mesh::grid(2, 2, 1.0f, 1.0f), 0.2f);
        particles.velocities[4] = Vec3{0.0f, -3.0f, 4.0f};
        const Aabb bounds = lateClothBounds(particles, 0.1f, 0.005f);
        assert(near(bounds.minimum.x, -0.5f - 0.505f, 1e-5f) && near(bounds.maximum.y, 0.5f + 0.505f, 1e-5f));
        assert(near(bounds.minimum.z, -0.505f, 1e-5f) && near(bounds.maximum.z, 0.505f, 1e-5f));

        const Aabb still = lateClothBounds(makeClothParticles(Mesh::grid(2, 2, 1.0f, 1.0f), 0.2f), 0.1f, 0.0f);
        assert(near(still.minimum.x, -0.5f, 1e-6f) && near(still.maximum.x, 0.5f, 1e-6f));
    }

    // A GPU cloth's frame entry: shaped on the GPU, no vertices, its late bounds, a shape fingerprint that is never 0.
    void testGpuDeformedMesh() {
        World world;
        const Entity sheet = addSheet(world, true, 7);
        const DeformedMesh deformed = gpuClothDeformedMesh(world.get<Cloth>(sheet), MeshHandle{7}, 0.05f, 12);
        assert(deformed.shapedOnGpu && deformed.vertices.empty());
        assert(deformed.mesh.index == 7);
        assert(deformed.shape != 0);
        assert(gpuClothDeformedMesh(world.get<Cloth>(sheet), MeshHandle{7}, 0.05f, 13).shape != deformed.shape);
        assert(deformed.bounds.minimum.x <= -0.5f && deformed.bounds.maximum.x >= 0.5f);
    }

    // The cloth lab steps its cloths where the engine says: on the GPU, its ticks queue.
    void testClothLabFollowsTheSolver() {
        ClothLabGame game;
        World world;
        AssetManager assets;
        game.onInit(world, assets);
        game.useClothSolver(ClothSolver::Gpu);
        runFixedUpdate(game, world, 1.0f / 120.0f, 1.0 / 120.0);

        int cloths = 0;
        world.each<Cloth>([&](Entity, const Cloth& cloth) {
            assert(cloth.onGpu && cloth.gpuTicks.size() == 1);
            ++cloths;
        });
        assert(cloths >= 2);
    }
}

namespace {
    GpuTimings clothTimed(double seconds) {
        GpuTimings timings;
        timings.valid = true;
        timings.passSeconds[static_cast<std::size_t>(GpuPass::Cloth)] = seconds;
        return timings;
    }

    /*
    * A cloth tick's GPU time, for the tick budget: a measured frame's cloth time over the ticks it ran; a frame not
    * measured, or one that ran no ticks, says nothing about a tick's cost and keeps the estimate.
    */
    void testGpuTickEstimate() {
        assert(gpuClothTickSeconds(0.0, clothTimed(0.016), 2) == 0.008);
        assert(gpuClothTickSeconds(0.008, clothTimed(0.003), 1) == 0.003);
        assert(gpuClothTickSeconds(0.008, GpuTimings{}, 3) == 0.008);
        assert(gpuClothTickSeconds(0.008, clothTimed(0.0004), 0) == 0.008);
        assert(gpuClothTickSeconds(0.0, GpuTimings{}, 0) == 0.0);
    }
}

void testGpuClothEngine() {
    testGpuTickEstimate();
    testDefaults();
    testTakeClothWork();
    testKeys();
    testApplyReadbacks();
    testLateBounds();
    testGpuDeformedMesh();
    testClothLabFollowsTheSolver();
}
