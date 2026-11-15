#include "math/mat4.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/still_instances.h"
#include "scene/camera.h"
#include "scene/frame_description.h"

#include "test_jobs.h"

#include <array>
#include <cassert>
#include <cstring>
#include <vector>

namespace {
    Camera testCamera() {
        return Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }

    DrawItem drawAt(float x, float roughness, std::uint32_t mesh) {
        DrawItem draw;
        draw.mesh = MeshHandle{mesh};
        draw.model = Mat4::translation(x, 1.0f, -2.0f);
        draw.previousModel = draw.model;
        draw.material.roughness = roughness;
        return draw;
    }

    // still still draws then moving ones, as a frame from kept draws lays them out, at this version after previous.
    FrameDescription frameOf(int still, int moving, std::uint64_t version, std::uint64_t previous, std::vector<DrawRange> built) {
        FrameDescription frame{.camera = testCamera()};

        for (int draw = 0; draw < still + moving; ++draw) {
            frame.draws.push_back(drawAt(static_cast<float>(draw), 0.1f * static_cast<float>(draw % 4), static_cast<std::uint32_t>(draw)));
        }

        frame.stillDraws = static_cast<std::size_t>(still);
        frame.builtStillDraws = std::move(built);
        frame.stillVersion = version;
        frame.stillPreviousVersion = previous;
        return frame;
    }

    // Two records alike once their materials are looked up in their own tables.
    bool sameRecord(const GpuInstance& a, const MaterialTable& aTable, const GpuInstance& b, const MaterialTable& bTable) {
        const GpuMaterial& aMaterial = aTable.getEntries().at(a.material[0]);
        const GpuMaterial& bMaterial = bTable.getEntries().at(b.material[0]);
        return std::memcmp(a.model, b.model, sizeof(a.model)) == 0 && std::memcmp(a.normalMatrix, b.normalMatrix, sizeof(a.normalMatrix)) == 0 &&
               std::memcmp(a.handedness, b.handedness, sizeof(a.handedness)) == 0 && a.material[1] == b.material[1] && a.material[2] == b.material[2] &&
               a.material[3] == b.material[3] && std::memcmp(&aMaterial, &bMaterial, sizeof(GpuMaterial)) == 0;
    }

    /*
    * The frame's records, kept still ones then the moving ones packed now (packMovingDraws), against every draw packed
    * from scratch: the same records, and every draw's last-frame matrix.
    */
    void assertMatchesScratch(const FrameDescription& frame, const StillInstances& still) {
        const SceneRecords records = packMovingDraws(frame, still, testJobs());
        MaterialTable scratchTable;
        const std::vector<GpuInstance> scratch = packInstances(frame.draws, scratchTable, testJobs());
        assert(still.getInstances().size() == frame.stillDraws && records.moving.size() == frame.draws.size() - frame.stillDraws);
        assert(still.getPreviousModels().size() == frame.stillDraws && records.movingPrevious.size() == records.moving.size());

        for (std::size_t draw = 0; draw < frame.draws.size(); ++draw) {
            const bool isStill = draw < frame.stillDraws;
            const GpuInstance& record = isStill ? still.getInstances()[draw] : records.moving[draw - frame.stillDraws];
            assert(sameRecord(record, records.materials, scratch[draw], scratchTable));

            float previous[16];
            writeColumnMajor(frame.draws[draw].previousModel.value_or(frame.draws[draw].model), previous);
            const std::array<float, 16>& kept = isStill ? still.getPreviousModels()[draw] : records.movingPrevious[draw - frame.stillDraws];
            assert(std::memcmp(kept.data(), previous, sizeof(previous)) == 0);
        }
    }

    std::size_t repackedCount(const std::vector<DrawRange>& ranges) {
        std::size_t count = 0;

        for (const DrawRange& range : ranges) {
            count += range.count;
        }

        return count;
    }

    /*
    * The first frame packs every still draw; the same frame again packs none; the next frame packs only the draws it
    * built; a frame that does not follow the last one packed (one was described but never drawn) packs them all.
    */
    void testOnlyBuiltDrawsArePacked() {
        StillInstances still;
        FrameDescription first = frameOf(5, 3, 1, 0, {DrawRange{0, 5}});
        assert(repackedCount(still.update(first, testJobs())) == 5);
        assertMatchesScratch(first, still);
        assert(still.update(first, testJobs()).empty());

        // Draw 2 changed: a new place and a material no draw had.
        FrameDescription second = frameOf(5, 3, 2, 1, {DrawRange{2, 1}});
        second.draws[2].model = Mat4::translation(9.0f, 0.0f, 0.0f);
        second.draws[2].previousModel = second.draws[2].model;
        second.draws[2].material.roughness = 0.77f;
        const std::vector<DrawRange> repacked = still.update(second, testJobs());
        assert(repacked.size() == 1 && repacked[0].first == 2 && repacked[0].count == 1);
        assertMatchesScratch(second, still);

        // Version 3 was never drawn.
        FrameDescription fourth = frameOf(5, 3, 4, 3, {});
        fourth.draws[2] = second.draws[2];
        assert(repackedCount(still.update(fourth, testJobs())) == 5);
        assertMatchesScratch(fourth, still);

        // Another count of still draws, or a whole build, packs them all.
        const FrameDescription fewer = frameOf(3, 4, 5, 4, {DrawRange{0, 3}});
        assert(repackedCount(still.update(fewer, testJobs())) == 3);
        assertMatchesScratch(fewer, still);
    }

    // Without still draws, everything is packed each frame as before.
    void testWithoutStillDraws() {
        StillInstances still;
        const FrameDescription frame = frameOf(0, 6, 0, 0, {});
        assert(still.update(frame, testJobs()).empty());
        assertMatchesScratch(frame, still);
    }

    // Changing one still draw's material again and again leaves old entries in the kept table, but only so many: past
    // them, everything is packed afresh into a clean table.
    void testTheTableStaysBounded() {
        StillInstances still;
        FrameDescription frame = frameOf(10, 2, 1, 0, {DrawRange{0, 10}});
        still.update(frame, testJobs());
        // At most one material more than at the start is in use (the changed draw's newest).
        const std::size_t live = still.getMaterials().getEntries().size() + 1;

        for (std::uint64_t version = 2; version < 300; ++version) {
            frame.stillVersion = version;
            frame.stillPreviousVersion = version - 1;
            frame.builtStillDraws = {DrawRange{4, 1}};
            frame.draws[4].material.roughness = 0.001f * static_cast<float>(version);
            still.update(frame, testJobs());
            assert(still.getMaterials().getEntries().size() <= 2 * live + 64);
        }

        assertMatchesScratch(frame, still);
    }
}

void testStillInstances() {
    testOnlyBuiltDrawsArePacked();
    testWithoutStillDraws();
    testTheTableStaysBounded();
}
