#include "core/command_registry.h"
#include "cull_helpers.h"
#include "engine/engine_commands.h"
#include "engine/settings_file.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/planar_reflection.h"
#include "render/gpu/reflection_probe.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/transparency.h"
#include "render/gpu/wind.h"
#include "scene/culling.h"
#include "scene/level_of_detail.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
    // Every mesh's place: a different count and place each, so commands name the right one; mesh 4 has buffers of its
    // own, as a deforming mesh does.
    MeshPlacement testPlacement(MeshHandle mesh) {
        if (mesh.index == 4) {
            return MeshPlacement{0, 100, 0, 15, false};
        }

        return MeshPlacement{1000 * mesh.index, 100, 5000 * mesh.index, 3 * (mesh.index + 1), true};
    }

    struct TestView {
        Frustum frustum;
        LevelView levels;
        WindSettings wind;
        bool motion = true;
    };

    // A camera somewhere over the scene looking about, levels and impostors on or off, the sway reaching 50-400 m.
    TestView randomView(std::mt19937& random, int index) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.6f + unit(random), 16.0f / 9.0f, 0.1f, 1000.0f + unit(random) * 4000.0f));
        const Vec3 eye{(unit(random) - 0.5f) * 2000.0f, 10.0f + unit(random) * 100.0f, (unit(random) - 0.5f) * 2000.0f};
        const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
        const Mat4 view = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});

        LevelOfDetailSettings settings;
        settings.enabled = index % 5 != 4;
        settings.impostors = index % 3 != 2;
        settings.pixelError = 0.5f + unit(random) * 4.0f;

        TestView test;
        test.frustum = Frustum::fromClipMatrix(projection * view, ClipDepth::OneToZero);
        test.levels = levelView(eye, 1080.0f + unit(random) * 1080.0f, 0.6f + unit(random) * 0.8f, settings);
        test.wind.sway = index % 4 != 3;
        test.wind.swayFadeEnd = 50.0f + unit(random) * 350.0f;
        test.motion = index % 6 != 5;
        return test;
    }

    std::vector<GpuDrawCommand> drawingCommands(const std::vector<GpuDrawCommand>& commands);

    // The view's record with the groups' drawing order's runs.
    GpuCameraCull packTestView(const TestView& view, const CullGroups& groups, const CullDrawOrder& order) {
        GpuCameraCull packed = packCameraCull(view.frustum, view.levels, view.motion, view.wind.enabled && view.wind.sway, view.wind.swayFadeEnd);
        packed.candidates = static_cast<std::uint32_t>(groups.candidates.size());
        packed.groups = static_cast<std::uint32_t>(groups.groups.size());
        packed.mainBase = 7;
        packed.motionBase = 70000;

        for (std::size_t run = 0; run < cullStyles; ++run) {
            packed.runFirst[run] = order.runFirst[run];
            packed.runCount[run] = order.runCount[run];
        }

        return packed;
    }

    /*
    * Each style's packed run holds, from its first place, exactly that run's commands drawing something, in drawing
    * order; with the groups drawn one by one (own buffers), every command drawing something is drawn once.
    */
    void assertPacked(const std::vector<GpuDrawCommand>& commands, const std::vector<GpuDrawCommand>& packed,
                      const std::array<std::uint32_t, cullStyles>& counts, const CullDrawOrder& order) {
        std::size_t drawn = 0;

        for (std::size_t run = 0; run < cullStyles; ++run) {
            std::uint32_t next = order.runFirst[run];

            for (std::uint32_t position = order.runFirst[run]; position < order.runFirst[run] + order.runCount[run]; ++position) {
                const GpuDrawCommand& command = commands[order.order[position]];

                if (command.instanceCount > 0) {
                    assert(packed[next].firstInstance == command.firstInstance && packed[next].instanceCount == command.instanceCount);
                    assert(packed[next].firstIndex == command.firstIndex && packed[next].vertexOffset == command.vertexOffset);
                    ++next;
                }
            }

            assert(next - order.runFirst[run] == counts[run]);
            drawn += counts[run];
        }

        for (std::size_t position = order.pooled; position < order.order.size(); ++position) {
            drawn += commands[order.order[position]].instanceCount > 0 ? 1 : 0;
        }

        assert(drawn == drawingCommands(commands).size());
    }

    // The draw list entries a CPU list gives (appendDrawList) and its batches as commands.
    std::vector<std::uint32_t> entriesOf(const BatchedDraws& batched) {
        std::vector<std::uint32_t> entries;
        appendDrawList(entries, batched);
        return entries;
    }

    // The commands drawing something, in group order.
    std::vector<GpuDrawCommand> drawingCommands(const std::vector<GpuDrawCommand>& commands) {
        std::vector<GpuDrawCommand> drawing;
        std::copy_if(commands.begin(), commands.end(), std::back_inserter(drawing), [](const GpuDrawCommand& command) { return command.instanceCount > 0; });
        return drawing;
    }

    // The batches as the GPU's commands: their mesh's index count, their first entry counted from base.
    void assertCommandsMatch(const std::vector<GpuDrawCommand>& commands, const BatchedDraws& batched, std::uint32_t base) {
        const std::vector<GpuDrawCommand> drawing = drawingCommands(commands);
        assert(drawing.size() == batched.batches.size());

        for (std::size_t batch = 0; batch < drawing.size(); ++batch) {
            const MeshPlacement placed = testPlacement(batched.batches[batch].mesh);
            assert(drawing[batch].indexCount == placed.indexCount);
            assert(drawing[batch].instanceCount == batched.batches[batch].instanceCount);
            assert(drawing[batch].firstInstance == base + batched.batches[batch].firstInstance);
            assert(drawing[batch].firstIndex == placed.firstIndex && drawing[batch].vertexOffset == static_cast<std::int32_t>(placed.firstVertex));
        }
    }

    /*
    * The passes' steps over every draw's candidates give batchByMesh's camera and velocity lists entry for entry (order
    * and thinned bits), its batches as commands and levelsChosen's counts, over cameras with levels, impostors, sway and
    * the velocity pass each on and off.
    */
    void testListsMatchTheCpu() {
        std::mt19937 random{43};
        const CullScene scene = randomCullScene(random);
        const std::vector<GpuMeshLevels> tables = packCullSceneLevels(scene);
        const CullGroups groups = buildCullGroups(scene.draws, 0, scene.draws.size(), testPlacement);
        const CullDrawOrder order = cullDrawOrder(groups.styles);
        std::vector<GpuCullRecord> records;

        for (const DrawItem& draw : scene.draws) {
            records.push_back(packCullRecord(draw));
        }

        std::size_t movingSeen = 0;

        for (int index = 0; index < 18; ++index) {
            const TestView view = randomView(random, index);
            const GpuCameraCull packed = packTestView(view, groups, order);
            const CameraCullResult result = cullCameraOnCpu(records, tables, groups.groups, groups.candidates, order.order, packed);
            assertPacked(result.mainCommands, result.packedMain, result.mainCounts, order);
            assertPacked(result.motionCommands, result.packedMotion, result.motionCounts, order);

            const std::vector<std::size_t> visible = opaqueDraws(scene.draws, visibleDraws(scene.draws, view.frustum));
            const BatchedDraws mainDraws = batchByMesh(scene.draws, visible, 0, &view.levels);
            assert(result.mainEntries == entriesOf(mainDraws));
            assertCommandsMatch(result.mainCommands, mainDraws, packed.mainBase);

            const std::array<std::size_t, meshLevelSlots> levels = levelsChosen(scene.draws, visible, view.levels);

            for (std::size_t slot = 0; slot < meshLevelSlots; ++slot) {
                assert(result.levels[slot] == levels[slot]);
            }

            std::vector<std::size_t> moving;

            for (std::size_t draw : visible) {
                const DrawItem& item = scene.draws[draw];
                const bool moved = item.previousModel && *item.previousModel != item.model;
                const bool sways = drawSways(item.sway, item.bounds.centre, item.bounds.radius, view.levels.eye, view.wind);

                if (view.motion && (moved || sways)) {
                    moving.push_back(draw);
                }
            }

            const BatchedDraws motionDraws = batchByMesh(scene.draws, moving, 0, &view.levels);
            assert(result.motionEntries == entriesOf(motionDraws));
            assertCommandsMatch(result.motionCommands, motionDraws, packed.motionBase);
            assert(result.mainCommands.size() == groups.groups.size() && result.motionCommands.size() == groups.groups.size());
            movingSeen += moving.size();
        }

        assert(movingSeen > 100);
    }

    /*
    * A capture's view (each probe face from its cycle's origin, and a mirror) through the passes' steps gives
    * batchByMesh's list over that view's own frustum and levels, entry for entry, and no velocity list: what the CPU
    * drew for each capture.
    */
    void testCaptureViewsMatchTheCpu() {
        std::mt19937 random{53};
        const CullScene scene = randomCullScene(random);
        const std::vector<GpuMeshLevels> tables = packCullSceneLevels(scene);
        const CullGroups groups = buildCullGroups(scene.draws, 0, scene.draws.size(), testPlacement);
        const CullDrawOrder order = cullDrawOrder(groups.styles);
        std::vector<GpuCullRecord> records;

        for (const DrawItem& draw : scene.draws) {
            records.push_back(packCullRecord(draw));
        }

        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        std::size_t drawn = 0;

        const auto assertViewMatches = [&](const CaptureCullView& capture) {
            GpuCameraCull packed = packCaptureCull(capture);
            assert((packed.flags & (CAMERA_CULL_MOTION | CAMERA_CULL_SWAY)) == 0u);
            packed.candidates = static_cast<std::uint32_t>(groups.candidates.size());
            packed.groups = static_cast<std::uint32_t>(groups.groups.size());
            packed.mainBase = 123;
            packed.motionBase = 123;

            for (std::size_t run = 0; run < cullStyles; ++run) {
                packed.runFirst[run] = order.runFirst[run];
                packed.runCount[run] = order.runCount[run];
            }

            const CameraCullResult result = cullCameraOnCpu(records, tables, groups.groups, groups.candidates, order.order, packed);
            const std::vector<std::size_t> visible = opaqueDraws(scene.draws, visibleDraws(scene.draws, capture.frustum));
            const BatchedDraws cpu = batchByMesh(scene.draws, visible, 0, &capture.levels);
            assert(result.mainEntries == entriesOf(cpu));
            assertCommandsMatch(result.mainCommands, cpu, packed.mainBase);
            assertPacked(result.mainCommands, result.packedMain, result.mainCounts, order);
            assert(result.motionEntries.empty() && drawingCommands(result.motionCommands).empty());
            drawn += result.mainEntries.size();
        };

        for (int round = 0; round < 3; ++round) {
            LevelOfDetailSettings settings;
            settings.enabled = round != 2;
            settings.pixelError = 0.5f + unit(random) * 4.0f;
            const Vec3 origin{(unit(random) - 0.5f) * 2000.0f, 10.0f + unit(random) * 100.0f, (unit(random) - 0.5f) * 2000.0f};

            for (int face = 0; face < cubeFaceCount; ++face) {
                assertViewMatches(probeFaceCullView(face, origin, 500.0f + unit(random) * 1500.0f, 128.0f + 384.0f * unit(random), settings));
            }

            // A mirror ahead of the camera facing it: the camera-relative view through it, as the renderer makes it.
            const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
            const Vec3 forward{std::cos(yaw), 0.0f, std::sin(yaw)};
            const MirrorPlane mirror{forward * -1.0f, (forward * -1.0f).dot(origin + forward * 20.0f)};
            const float fov = 0.6f + unit(random) * 0.8f;
            const Mat4 relative = toGpuDepthRange(Mat4::perspective(fov, 16.0f / 9.0f, 0.1f, 3000.0f)) *
                                  Mat4::lookAt(Vec3{}, forward, Vec3{0.0f, 1.0f, 0.0f});
            const Mat4 mirrorView = planarViewProjection(relative, planeRelativeTo(mirror, origin));
            assertViewMatches(mirrorCullView(mirrorView, origin, mirror, 540.0f, fov, settings));
        }

        assert(drawn > 1000);
    }

    /*
    * Views placed one after another in the passes' shared buffers: each takes its counts, the drawing order's runs and
    * slices as big as one view needs, the next view's starting where its own end, so no two views share a word.
    */
    void testViewsHaveTheirOwnSlices() {
        std::mt19937 random{61};
        const CullScene scene = randomCullScene(random, 1000);
        const CullGroups groups = buildCullGroups(scene.draws, 0, scene.draws.size(), testPlacement);
        const CullDrawOrder order = cullDrawOrder(groups.styles);
        const std::uint32_t candidates = static_cast<std::uint32_t>(groups.candidates.size());
        const std::uint32_t blocks = (candidates + cameraCullGroupSize - 1) / cameraCullGroupSize;

        GpuCameraCull first{};
        GpuCameraCull second{};
        placeCullView(first, 0, candidates, order);
        placeCullView(second, 1, candidates, order);
        assert(first.candidates == candidates && first.groups == order.order.size() && first.blocks == blocks);
        assert(first.view == 0 && second.view == 1);

        for (std::size_t run = 0; run < cullStyles; ++run) {
            assert(second.runFirst[run] == order.runFirst[run] && second.runCount[run] == order.runCount[run]);
        }

        assert(first.slices[CAMERA_CULL_SLICE_FLAGS] == 0 && second.slices[CAMERA_CULL_SLICE_FLAGS] == candidates);
        assert(first.slices[CAMERA_CULL_SLICE_BLOCKS] == 0 && second.slices[CAMERA_CULL_SLICE_BLOCKS] == blocks + 1);
        assert(first.slices[CAMERA_CULL_SLICE_COMMANDS] == 0 && second.slices[CAMERA_CULL_SLICE_COMMANDS] == first.groups);
        assert(first.slices[CAMERA_CULL_SLICE_PLACES] == 0 && second.slices[CAMERA_CULL_SLICE_PLACES] == order.pooled + 1);

        // What every view's slices take together, for sizing the buffers.
        const CullViewSizes sizes = cullViewSizes(2, candidates, order);
        assert(sizes.flags == 2 * candidates && sizes.blocks == 2 * (blocks + 1));
        assert(sizes.commands == 2 * first.groups && sizes.places == 2 * (order.pooled + 1));
    }

    /*
    * Groups follow batchByMesh's order (style, then mesh), each group's candidates in draw order; glass draws none, and
    * a draw has one candidate a slot with a mesh. Draw numbers count from the range's first.
    */
    void testGroupsFollowTheBatchOrder() {
        std::mt19937 random{47};
        const CullScene scene = randomCullScene(random, 600);
        const CullGroups groups = buildCullGroups(scene.draws, 100, 400, testPlacement);
        assert(groups.groups.size() == groups.styles.size());

        std::size_t candidates = 0;

        for (std::size_t draw = 100; draw < 500; ++draw) {
            const DrawItem& item = scene.draws[draw];

            if (isTransparent(item.material)) {
                continue;
            }

            ++candidates;

            for (const MeshLevel& level : item.levels) {
                candidates += level.mesh.isValid() ? 1 : 0;
            }
        }

        assert(groups.candidates.size() == candidates);
        std::uint32_t next = 0;

        for (std::size_t group = 0; group < groups.groups.size(); ++group) {
            const GpuCullGroup& g = groups.groups[group];
            const CullGroupStyle& style = groups.styles[group];
            assert(g.firstCandidate == next && g.candidates > 0);
            assert(g.indexCount == testPlacement(style.mesh).indexCount && g.firstIndex == testPlacement(style.mesh).firstIndex);
            assert(style.pooled == testPlacement(style.mesh).pooled);
            next += g.candidates;

            if (group > 0) {
                const CullGroupStyle& before = groups.styles[group - 1];
                const int styleBefore = (before.cutOut ? 2 : 0) + (before.doubleSided ? 1 : 0);
                const int styleNow = (style.cutOut ? 2 : 0) + (style.doubleSided ? 1 : 0);
                assert(styleBefore < styleNow || (styleBefore == styleNow && before.mesh.index < style.mesh.index));
            }

            for (std::uint32_t c = g.firstCandidate; c < g.firstCandidate + g.candidates; ++c) {
                const GpuCullCandidate& candidate = groups.candidates[c];
                assert(candidate.draw >= 100 && candidate.draw < 500);
                assert(c == g.firstCandidate || groups.candidates[c - 1].draw < candidate.draw);
                const DrawItem& item = scene.draws[candidate.draw];
                const MeshHandle mesh = candidate.slot == 0 ? item.mesh : item.levels[candidate.slot - 1].mesh;
                assert(mesh == style.mesh && isCutOut(item.material) == style.cutOut && item.material.doubleSided == style.doubleSided);
            }
        }

        assert(next == groups.candidates.size());
    }

    // A frame of still draws (the first stillCount) and moving ones, at a version following the one before.
    FrameDescription stillFrame(const CullScene& scene, std::size_t stillCount, std::uint64_t version, std::uint64_t previous) {
        FrameDescription frame{.camera = Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f}};
        frame.draws = scene.draws;
        frame.stillDraws = stillCount;
        frame.stillVersion = version;
        frame.stillPreviousVersion = previous;
        return frame;
    }

    /*
    * The still part is kept: the same version builds nothing, a following one with a built range of moved bounds
    * repacks records but keeps the groups, a range whose mesh changed builds them again, a frame not following builds
    * them afresh. The moving draws' groups come after, their candidates after the still ones; drawing order goes by
    * style then mesh; level tables say which meshes deform; glass and flat-faced draws are listed.
    */
    void testTableKeepsTheStillDraws() {
        std::mt19937 random{53};
        CullScene scene = randomCullScene(random, 800);
        const std::vector<FlatFace> faces(1);
        scene.draws[3].flatFaces = faces;
        scene.draws[650].flatFaces = faces;
        scene.draws[3].material.alphaMode = AlphaMode::Opaque;
        scene.draws[650].material.alphaMode = AlphaMode::Opaque;

        CameraCullTable table;
        const auto deforms = [](MeshHandle mesh) { return mesh.index == 5; };
        CameraCullFrame first = table.update(stillFrame(scene, 600, 1, 0), testPlacement, deforms);
        assert(table.getStillBuilds() == 1);
        assert(first.stillRecords.size() == 600 && first.movingRecords.size() == 200);
        assert(first.meshLevels.size() == cullSceneMeshes && first.meshLevels[5].changesShape == 1 && first.meshLevels[4].changesShape == 0);
        assert(std::find(first.flatFaced.begin(), first.flatFaced.end(), 3) != first.flatFaced.end());
        assert(std::find(first.flatFaced.begin(), first.flatFaced.end(), 650) != first.flatFaced.end());

        for (std::size_t draw = 0; draw < scene.draws.size(); ++draw) {
            const bool listed = std::find(first.glass.begin(), first.glass.end(), draw) != first.glass.end();
            assert(listed == isTransparent(scene.draws[draw].material));
        }

        // The still candidates first, then the moving ones; every group's candidates within its own part.
        const CullGroups stillGroups = buildCullGroups(scene.draws, 0, 600, testPlacement);
        assert(first.stillCandidates.size() == stillGroups.candidates.size());
        assert(first.groups.size() > stillGroups.groups.size());

        for (std::size_t group = stillGroups.groups.size(); group < first.groups.size(); ++group) {
            assert(first.groups[group].firstCandidate >= first.stillCandidates.size());
        }

        for (const GpuCullCandidate& candidate : first.movingCandidates) {
            assert(candidate.draw >= 600);
        }

        // Drawing order: pooled groups first by style then mesh, each style a run; the own-buffer groups after.
        const CullDrawOrder& order = first.drawOrder;
        assert(order.order.size() == first.groups.size());
        assert(order.pooled > 0 && order.pooled < order.order.size());

        for (std::size_t position = 1; position < order.order.size(); ++position) {
            const CullGroupStyle& before = first.styles[order.order[position - 1]];
            const CullGroupStyle& now = first.styles[order.order[position]];
            const int styleBefore = (before.cutOut ? 2 : 0) + (before.doubleSided ? 1 : 0) + (before.pooled ? 0 : 4);
            const int styleNow = (now.cutOut ? 2 : 0) + (now.doubleSided ? 1 : 0) + (now.pooled ? 0 : 4);
            assert(styleBefore < styleNow || (styleBefore == styleNow && before.mesh.index <= now.mesh.index));
        }

        std::uint32_t runs = 0;

        for (std::size_t run = 0; run < cullStyles; ++run) {
            for (std::uint32_t position = order.runFirst[run]; position < order.runFirst[run] + order.runCount[run]; ++position) {
                const CullGroupStyle& style = first.styles[order.order[position]];
                assert(style.pooled && static_cast<std::size_t>((style.cutOut ? 2 : 0) + (style.doubleSided ? 1 : 0)) == run);
            }

            runs += order.runCount[run];
        }

        assert(runs == order.pooled);

        const std::uint64_t generation = first.stillGeneration;
        CameraCullFrame again = table.update(stillFrame(scene, 600, 1, 0), testPlacement, deforms);
        assert(table.getStillBuilds() == 1 && again.stillGeneration == generation);

        // The still records each frame packed again, for the uploads (scene-cells): all of them first, none kept.
        assert(first.stillRepacked.size() == 1 && first.stillRepacked[0].first == 0 && first.stillRepacked[0].count == 600);
        assert(again.stillRepacked.empty());

        // A built range whose bounds moved: records repacked, groups kept.
        scene.draws[10].bounds.centre.x += 5.0f;
        FrameDescription moved = stillFrame(scene, 600, 2, 1);
        moved.builtStillDraws = {DrawRange{10, 1}};
        CameraCullFrame repacked = table.update(moved, testPlacement, deforms);
        assert(table.getStillBuilds() == 1 && repacked.stillGeneration != generation);
        assert(repacked.stillRecords[10].bounds[0] == scene.draws[10].bounds.centre.x);
        assert(repacked.stillRepacked.size() == 1 && repacked.stillRepacked[0].first == 10 && repacked.stillRepacked[0].count == 1);

        // A built range whose mesh changed: groups built again.
        scene.draws[11].mesh = MeshHandle{(scene.draws[11].mesh.index + 1) % cullSceneMeshes};
        scene.draws[11].levels = scene.levelsByMesh[scene.draws[11].mesh.index];
        FrameDescription remeshed = stillFrame(scene, 600, 3, 2);
        remeshed.builtStillDraws = {DrawRange{11, 1}};
        const CameraCullFrame regrouped = table.update(remeshed, testPlacement, deforms);
        assert(table.getStillBuilds() == 2);
        assert(regrouped.stillRepacked.size() == 1 && regrouped.stillRepacked[0].count == 600);

        // Not following: afresh.
        table.update(stillFrame(scene, 600, 9, 7), testPlacement, deforms);
        assert(table.getStillBuilds() == 3);
    }

    /*
    * The same list compares equal; an entry dropped is missing, one added extra, a thinned bit flipped both; draws near
    * an edge are counted apart; the reply says whether they agree.
    */
    void testComparison() {
        std::mt19937 random{59};
        const CullScene scene = randomCullScene(random, 1000);
        const CullGroups groups = buildCullGroups(scene.draws, 0, scene.draws.size(), testPlacement);
        const CullDrawOrder order = cullDrawOrder(groups.styles);
        const std::vector<GpuMeshLevels> tables = packCullSceneLevels(scene);
        std::vector<GpuCullRecord> records;

        for (const DrawItem& draw : scene.draws) {
            records.push_back(packCullRecord(draw));
        }

        const TestView view = randomView(random, 0);
        const GpuCameraCull packed = packTestView(view, groups, order);
        const CameraCullResult result = cullCameraOnCpu(records, tables, groups.groups, groups.candidates, order.order, packed);
        const BatchedDraws cpu = batchByMesh(scene.draws, opaqueDraws(scene.draws, visibleDraws(scene.draws, view.frustum)), 0, &view.levels);
        const auto never = [](std::uint32_t) { return false; };

        const CullListComparison same = compareCullList(groups.styles, result.mainCommands, result.mainEntries, packed.mainBase, cpu, never);
        assert(same.entries == cpu.order.size() && same.matching == same.entries && same.missing == 0 && same.extra == 0);
        const std::vector<NamedCullComparison> agreeing{{"Camera", same}, {"Velocity", same}};
        assert(describeCullComparison(agreeing).find("agree") != std::string::npos);

        // The last entry of the first drawing group changed to another draw: one missing, one extra.
        std::vector<std::uint32_t> changed = result.mainEntries;
        const auto drawing = std::find_if(result.mainCommands.begin(), result.mainCommands.end(), [](const GpuDrawCommand& c) { return c.instanceCount > 0; });
        assert(drawing != result.mainCommands.end());
        const std::uint32_t at = drawing->firstInstance - packed.mainBase + drawing->instanceCount - 1;
        changed[at] = (changed[at] & drawListIndexMask) == 999 ? 998 : 999;
        const CullListComparison differs = compareCullList(groups.styles, result.mainCommands, changed, packed.mainBase, cpu, never);
        assert(differs.missing == 1 && differs.extra == 1 && differs.matching == differs.entries - 1);
        const std::vector<NamedCullComparison> faceDiffers{{"Camera", same}, {"Probe face 2", differs}};
        const std::string differing = describeCullComparison(faceDiffers);
        assert(differing.find("DIFFER") != std::string::npos && differing.find("Probe face 2: ") != std::string::npos);

        // Every draw near an edge: the differences are counted there, and the lists agree.
        const auto always = [](std::uint32_t) { return true; };
        const CullListComparison edges = compareCullList(groups.styles, result.mainCommands, changed, packed.mainBase, cpu, always);
        assert(edges.nearAnEdge == 2);
        const std::vector<NamedCullComparison> nearEdges{{"Camera", edges}, {"Mirror", same}};
        assert(describeCullComparison(nearEdges).find("agree") != std::string::npos);

        // The thinned bit counts.
        changed = result.mainEntries;
        changed[at] ^= drawListThinnedBit;
        const CullListComparison thinned = compareCullList(groups.styles, result.mainCommands, changed, packed.mainBase, cpu, never);
        assert(thinned.missing == 1 && thinned.extra == 1);
    }

    // A level's error at the budget, or a sphere touching a plane, is near an edge; well inside, not.
    void testEdges() {
        DrawItem draw;
        draw.mesh = MeshHandle{0};
        draw.bounds = BoundingSphere{Vec3{0.0f, 0.0f, -100.0f}, 1.0f};
        const std::vector<MeshLevel> levels{MeshLevel{MeshHandle{1}, 0.5f, false}};
        draw.levels = levels;
        draw.sway.kind = SwayKind::Tree;

        LevelOfDetailSettings settings;
        LevelView view = levelView(Vec3{0.0f, 0.0f, 0.0f}, 1000.0f, 1.0f, settings);
        const Frustum frustum = Frustum::fromClipMatrix(toGpuDepthRange(Mat4::perspective(1.0f, 1.0f, 0.1f, 1000.0f)), ClipDepth::OneToZero);
        const GpuCullRecord record = packCullRecord(draw);
        const GpuMeshLevels table = packMeshLevels(draw.mesh, levels);
        assert(!nearACameraEdge(record, table, packCameraCull(frustum, view, true, false, 100.0f)));

        // The budget exactly the level's error in pixels 99 m away.
        view.pixelError = errorInPixels(0.5f, 99.0f, view);
        assert(nearACameraEdge(record, table, packCameraCull(frustum, view, true, false, 100.0f)));

        // The sway reaching exactly to it.
        view = levelView(Vec3{0.0f, 0.0f, 0.0f}, 1000.0f, 1.0f, settings);
        assert(nearACameraEdge(record, table, packCameraCull(frustum, view, true, true, 99.0f)));
    }

    // GPU culling is the only way, so there is no setting for it: an old settings line is warned about.
    void testSetting() {
        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) { visit(settings, visitor); };
        const std::vector<std::string> warnings = applySettings({{"passes.gpuCulling", "false"}}, walk, StartupFields::Keep);
        assert(warnings.size() == 1);
    }

    // Each cull-compare call asks for a comparison and shows the last one; it takes no arguments.
    void testCompareCommand() {
        CommandRegistry registry;
        int asked = 0;
        std::string last;
        addCullCompareCommand(registry, [&]() {
            ++asked;
            return last;
        });

        assert(!registry.execute("cull-compare now").ok && asked == 0);

        const CommandReply first = registry.execute("cull-compare");
        assert(first.ok && asked == 1 && first.text.find("No comparison") != std::string::npos);

        last = "The GPU's lists agree with the CPU's.";
        const CommandReply second = registry.execute("cull-compare");
        assert(second.ok && asked == 2 && second.text.find("agree") != std::string::npos);
    }
}

/*
* The camera's lists built by the GPU's steps (render/gpu/camera_culling.h): equal to the CPU's batchByMesh lists, the
* still table kept, and cull-compare's comparison.
*/
void testCameraCulling() {
    testListsMatchTheCpu();
    testCaptureViewsMatchTheCpu();
    testViewsHaveTheirOwnSlices();
    testGroupsFollowTheBatchOrder();
    testTableKeepsTheStillDraws();
    testComparison();
    testEdges();
    testSetting();
    testCompareCommand();
}
