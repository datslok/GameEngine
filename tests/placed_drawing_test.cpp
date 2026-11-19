#include "math/frustum.h"
#include "math/transform.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/level_fade.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/transparency.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/level_of_detail.h"

#include "planting_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <random>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
    bool nearlyRelative(float a, float b, float share) {
        return std::fabs(a - b) <= share * std::max({std::fabs(a), std::fabs(b), 1.0f});
    }

    // Every mesh in the pools, a different place each, so commands name the right one; mesh 99 has buffers of its own.
    MeshPlacement testPlacement(MeshHandle mesh) {
        if (mesh.index == 99) {
            return MeshPlacement{0, 100, 0, 15, false};
        }

        return MeshPlacement{1000 * mesh.index, 100, 5000 * mesh.index, 3 * (mesh.index + 1), true};
    }

    // The meshes' levels, kept for the draws' spans: a tree's bark (a coarser level, the shadow's stand-in, nothing past
    // the card's error), its leaves (the same, the first level sharing the leaves' vertices), the card (no levels).
    const std::vector<MeshLevel>& barkLevels() {
        static const std::vector<MeshLevel> levels{MeshLevel{MeshHandle{11}, 0.02f}, MeshLevel{MeshHandle{12}, 0.08f}, MeshLevel{MeshHandle{}, 0.4f}};
        return levels;
    }

    const std::vector<MeshLevel>& leafLevels() {
        static const std::vector<MeshLevel> levels{MeshLevel{MeshHandle{21}, 0.03f, true}, MeshLevel{MeshHandle{22}, 0.1f}, MeshLevel{MeshHandle{}, 0.4f}};
        return levels;
    }

    // A part's mesh box and the tree's box its level is chosen by, in the plant's frame.
    const Aabb partBox{Vec3{-2.0f, 0.0f, -2.0f}, Vec3{2.5f, 18.0f, 1.5f}};
    const Aabb treeBox{Vec3{-6.0f, 0.0f, -5.0f}, Vec3{6.0f, 22.0f, 7.0f}};

    // A part as the engine attaches it in the plant's frame (the identity): bounds, levels, options and entries.
    DrawItem treePart(MeshHandle mesh, const std::vector<MeshLevel>* levels, bool cutOut, bool doubleSided, float hiddenBelowError, std::uint32_t thinning) {
        DrawItem part;
        part.mesh = mesh;
        part.castsShadows = false;
        part.material.alphaMode = cutOut ? AlphaMode::Mask : AlphaMode::Opaque;
        part.material.doubleSided = doubleSided;
        part.material.roughness = 0.1f * static_cast<float>(mesh.index % 7);

        if (levels != nullptr) {
            part.levels = *levels;
        }

        part.hiddenBelowError = hiddenBelowError;
        part.fadesLevels = true;
        part.thinningEntry = thinning;
        part.bounds = boundingSphere(partBox, part.model);
        part.levelBounds = boundingSphere(treeBox, part.model);
        return part;
    }

    /*
    * Two kinds of layer 0 (a tree of bark, leaves and card; one of bark and card) and one of layer 1 (leaves only);
    * layer 2 not drawn. Styles mixed so the parts are sorted.
    */
    struct TestKinds {
        std::vector<DrawItem> parts;
        std::vector<PlantedKindParts> kinds;
    };

    TestKinds testKinds() {
        TestKinds test;
        const auto kind = [&](int layer, int which, std::vector<DrawItem> parts) {
            test.kinds.push_back(PlantedKindParts{.layer = layer, .kind = which, .first = test.parts.size(), .count = parts.size()});
            test.parts.insert(test.parts.end(), parts.begin(), parts.end());
        };
        kind(0, 0, {treePart(MeshHandle{10}, &barkLevels(), false, false, 0.0f, 0), treePart(MeshHandle{20}, &leafLevels(), true, true, 0.0f, 5),
                    treePart(MeshHandle{30}, nullptr, true, false, 0.4f, 0)});
        kind(0, 1, {treePart(MeshHandle{10}, &barkLevels(), false, false, 0.0f, 0), treePart(MeshHandle{31}, nullptr, true, false, 0.4f, 0)});
        kind(1, 0, {treePart(MeshHandle{20}, &leafLevels(), true, true, 0.0f, 5)});
        return test;
    }

    GpuPlacedPlant randomPlant(std::mt19937& random, const PlantingBox& box) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        GpuPlacedPlant plant{};
        plant.foot[0] = box.west + unit(random) * (box.east - box.west);
        plant.foot[1] = (unit(random) - 0.5f) * 20.0f;
        plant.foot[2] = box.north + unit(random) * (box.south - box.north);
        plant.scale = 0.8f + unit(random) * 0.4f;
        plant.turnRadians = (unit(random) - 0.3f) * 4.0f * std::numbers::pi_v<float>;
        plant.kind = static_cast<std::uint32_t>(unit(random) * 4.0f) % 4u;
        return plant;
    }

    Transform transformOf(const GpuPlacedPlant& plant) {
        return Transform{.position = Vec3{plant.foot[0], plant.foot[1], plant.foot[2]},
                         .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, plant.turnRadians),
                         .scale = Vec3{plant.scale, plant.scale, plant.scale}};
    }

    // The part drawn as an entity would draw it: placed by the plant's Transform and attached there.
    DrawItem entityPart(const DrawItem& part, const GpuPlacedPlant& plant, const Aabb& box) {
        DrawItem entity = part;
        entity.model = transformOf(plant).getMatrix();
        entity.bounds = boundingSphere(box, entity.model);
        entity.levelBounds = boundingSphere(treeBox, entity.model);
        return entity;
    }

    // Slots of four cells round the path's south, every layer's plants random in their cells, counts below their room.
    struct TestSlots {
        PlantingSlotLayout layout;
        std::map<PlantingCell, int> held;
        std::vector<GpuPlacedPlant> plants;
        std::vector<std::uint32_t> counts;
        std::size_t layers = 0;
    };

    TestSlots testSlots(std::mt19937& random) {
        const std::vector<PlantingLayerRecord> layers = threeLayers();
        TestSlots slots;
        slots.layers = layers.size();
        slots.layout = plantingSlotLayout(layers, 64.0f);
        slots.held = {{PlantingCell{-1, -3}, 3}, {PlantingCell{0, -3}, 0}, {PlantingCell{-1, -2}, 5}, {PlantingCell{0, -2}, 1}};
        constexpr int slotCount = 6;
        slots.plants.resize(static_cast<std::size_t>(slotCount) * slots.layout.plantsASlot);
        slots.counts.assign(static_cast<std::size_t>(slotCount) * slots.layers, 0u);

        for (const auto& [cell, slot] : slots.held) {
            const PlantingBox box = plantingCellBox(cell, 64.0f);

            for (std::size_t layer = 0; layer < slots.layers; ++layer) {
                std::uniform_int_distribution<std::uint32_t> counted{0, std::min<std::uint32_t>(slots.layout.layerCapacity[layer], 150u)};
                const std::uint32_t count = counted(random);
                slots.counts[static_cast<std::size_t>(slot) * slots.layers + layer] = count;

                for (std::uint32_t plant = 0; plant < count; ++plant) {
                    slots.plants[static_cast<std::size_t>(slot) * slots.layout.plantsASlot + slots.layout.layerFirst[layer] + plant] = randomPlant(random, box);
                }
            }
        }

        // A dropped cell's stale plants in slot 2 are never read: it is not held.
        slots.counts[2 * slots.layers] = 40;
        return slots;
    }

    // A view as the CPU's functions take it, and packed.
    struct TestCamera {
        Frustum frustum;
        LevelView levels;
        GpuCameraCull packed;
    };

    TestCamera randomCamera(std::mt19937& random, int index) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.6f + unit(random), 16.0f / 9.0f, 0.1f, 2000.0f));
        const Vec3 eye{(unit(random) - 0.5f) * 200.0f, 2.0f + unit(random) * 60.0f, -60.0f - unit(random) * 200.0f};
        const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
        const Mat4 view = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.2f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
        LevelOfDetailSettings settings;
        settings.enabled = index % 5 != 4;
        settings.impostors = index % 3 != 2;
        settings.pixelError = 0.5f + unit(random) * 4.0f;
        settings.fadeBand = index % 3 == 1 ? 0.0f : 0.05f + unit(random) * 0.4f;
        const Frustum frustum = Frustum::fromClipMatrix(projection * view, ClipDepth::OneToZero);
        const LevelView levels = cameraLevelView(eye, 1080.0f, 0.9f, settings);
        return TestCamera{frustum, levels, packCameraCull(frustum, levels, index % 2 == 0, false, 0.0f)};
    }

    // Looking down on the slots from 300 m, every mesh at its finest (levels off): every plant of a drawn kind draws.
    GpuCameraCull overheadCamera() {
        LevelOfDetailSettings settings;
        settings.enabled = false;
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(1.4f, 1.0f, 0.1f, 2000.0f));
        const Vec3 eye{0.0f, 300.0f, -120.0f};
        const Frustum frustum = Frustum::fromClipMatrix(projection * Mat4::lookAt(eye, Vec3{0.0f, 0.0f, -120.0f}, Vec3{0.0f, 0.0f, -1.0f}), ClipDepth::OneToZero);
        return packCameraCull(frustum, cameraLevelView(eye, 1080.0f, 1.4f, settings), false, false, 0.0f);
    }

    const PlantedRegion testRegion{.northmost = -170.0f, .southmost = -70.0f, .spread = 50.0f};

    // An entry's place: its group's mesh and style, its plant and part, its draw list bits.
    using Key = std::tuple<std::uint32_t, int, std::uint32_t, std::uint32_t, std::uint32_t>;

    int styleNumber(bool cutOut, bool doubleSided) {
        return (cutOut ? 2 : 0) + (doubleSided ? 1 : 0);
    }

    void testTheModelIsTheEntitys() {
        std::mt19937 random{3};

        for (int test = 0; test < 2000; ++test) {
            const GpuPlacedPlant plant = randomPlant(random, PlantingBox{-3000.0f, 3000.0f, -3000.0f, 3000.0f});
            const Mat4 placed = placedPlantModel(plant);
            const Mat4 entity = transformOf(plant).getMatrix();

            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    assert(placed.values[row][column] == entity.values[row][column]);
                }
            }
        }
    }

    void testThePartsAreTheEntitys() {
        std::mt19937 random{5};
        const TestKinds kinds = testKinds();

        for (int test = 0; test < 500; ++test) {
            const GpuPlacedPlant plant = randomPlant(random, PlantingBox{-3000.0f, 3000.0f, -3000.0f, 3000.0f});

            for (const DrawItem& part : kinds.parts) {
                const DrawItem entity = entityPart(part, plant, partBox);
                const GpuCullRecord placed = placedCullRecord(packCullRecord(part), plant);
                const GpuCullRecord expected = packCullRecord(entity);
                assert(placed.flags == expected.flags && placed.mesh == expected.mesh && placed.hiddenBelowError == expected.hiddenBelowError &&
                       placed.shadowCascades == expected.shadowCascades);

                for (int axis = 0; axis < 4; ++axis) {
                    assert(nearlyRelative(placed.bounds[axis], expected.bounds[axis], 2e-6f));
                    assert(nearlyRelative(placed.levelSphere[axis], expected.levelSphere[axis], 2e-6f));
                }

                const GpuInstance instance = placedInstance(packInstance(part, 7), plant);
                const GpuInstance wanted = packInstance(entity, 7);

                for (int value = 0; value < 16; ++value) {
                    assert(instance.model[value] == wanted.model[value]);
                }

                // The rotation alone against the cofactors (scale squared times it): the same directions.
                for (int column = 0; column < 3; ++column) {
                    for (int row = 0; row < 3; ++row) {
                        const int value = column * 4 + row;
                        assert(std::fabs(instance.normalMatrix[value] * plant.scale * plant.scale - wanted.normalMatrix[value]) <= 1e-5f);
                    }
                }

                assert(instance.handedness[0] == wanted.handedness[0] && nearlyRelative(instance.handedness[1], wanted.handedness[1], 1e-6f));

                for (int value = 0; value < 4; ++value) {
                    assert(instance.material[value] == wanted.material[value] && instance.sway[value] == wanted.sway[value] &&
                           instance.hanging[value] == wanted.hanging[value]);
                    assert(nearlyRelative(instance.levelSphere[value], wanted.levelSphere[value], 2e-6f));
                }
            }
        }
    }

    void testTheRegion() {
        PlantingMaps maps = straightMaps();
        assert(insidePlantedRegion(maps, testRegion, Vec3{49.0f, 3.0f, -100.0f}));
        assert(insidePlantedRegion(maps, testRegion, Vec3{-50.0f, 3.0f, -170.0f}));
        assert(!insidePlantedRegion(maps, testRegion, Vec3{50.5f, 3.0f, -100.0f}));
        assert(!insidePlantedRegion(maps, testRegion, Vec3{0.0f, 3.0f, -171.0f}));
        assert(!insidePlantedRegion(maps, testRegion, Vec3{0.0f, 3.0f, -69.0f}));

        // A winding axis: the region follows the maps' axis, not x = 0.
        for (std::size_t row = 0; row < maps.axis.size(); ++row) {
            maps.axis[row] = 0.5f * static_cast<float>(row);
        }

        for (float z = -170.0f; z <= -70.0f; z += 7.3f) {
            const float axis = plantingAxisAt(maps, z);
            assert(insidePlantedRegion(maps, testRegion, Vec3{axis + 49.9f, 0.0f, z}));
            assert(!insidePlantedRegion(maps, testRegion, Vec3{axis - 50.1f, 0.0f, z}));
        }
    }

    void testTheTable() {
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement);
        assert(table.parts.size() == kinds.parts.size() && table.draws.size() == kinds.parts.size());
        assert(table.kinds.size() == 3 * static_cast<std::size_t>(plantingMostKinds));
        assert(table.drawnLayers == (std::vector<bool>{true, true, false}));
        assert(table.mostParts == 3);

        // Parts by style, each pointing at its draw; each kind's parts its draws, in order within a style.
        for (std::size_t part = 0; part < table.parts.size(); ++part) {
            const DrawItem& draw = kinds.parts[table.draws[part]];
            assert(table.parts[part].record.mesh == draw.mesh.index);
            const int style = styleNumber(isCutOut(draw.material), draw.material.doubleSided);

            if (part > 0) {
                const DrawItem& before = kinds.parts[table.draws[part - 1]];
                assert(styleNumber(isCutOut(before.material), before.material.doubleSided) <= style);
            }

            // Its groups: a level slot each, the slot's mesh in the commands' place; a meshless one draws nothing.
            const GpuPlacedPart& placed = table.parts[part];

            for (std::uint32_t slot = 0; slot <= placed.levels.levels; ++slot) {
                const GpuPlacedGroup& group = table.groups[placed.firstGroup + slot];
                const CullGroupStyle& groupStyle = table.styles[placed.firstGroup + slot];
                const MeshHandle mesh = slot == 0 ? draw.mesh : draw.levels[slot - 1].mesh;
                assert(groupStyle.mesh == mesh && groupStyle.cutOut == isCutOut(draw.material) && groupStyle.doubleSided == draw.material.doubleSided);
                assert(group.indexCount == (mesh.isValid() ? testPlacement(mesh).indexCount : 0u));
                assert(!mesh.isValid() || group.firstIndex == testPlacement(mesh).firstIndex);
            }
        }

        for (const PlantedKindParts& kind : kinds.kinds) {
            const GpuPlacedKind& placed = table.kinds[static_cast<std::size_t>(kind.layer * plantingMostKinds + kind.kind)];
            assert(placed.parts == kind.count);

            for (std::uint32_t part = 0; part < placed.parts; ++part) {
                assert(table.draws[placed.part[part]] == kind.first + part);
            }
        }

        assert(table.kinds[1 * plantingMostKinds + 1].parts == 0);

        // The groups' order: every group pooled, style runs covering them.
        assert(table.order.pooled == table.groups.size() && table.order.order.size() == table.groups.size());
        std::uint32_t covered = 0;

        for (std::size_t style = 0; style < cullStyles; ++style) {
            assert(table.order.runFirst[style] == covered || table.order.runCount[style] == 0);
            covered += table.order.runCount[style];
        }

        assert(covered == table.groups.size());

        // What the table refuses.
        const auto refuses = [&](std::vector<DrawItem> parts, std::vector<PlantedKindParts> runs) {
            try {
                buildPlacedTable(parts, runs, 3, testPlacement);
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };
        const DrawItem plain = kinds.parts.front();
        DrawItem glass = plain;
        glass.material.alphaMode = AlphaMode::Blend;
        DrawItem unpooled = plain;
        unpooled.mesh = MeshHandle{99};
        unpooled.levels = {};
        assert(refuses({glass}, {PlantedKindParts{0, 0, 0, 1}}));
        assert(refuses({unpooled}, {PlantedKindParts{0, 0, 0, 1}}));
        assert(refuses({plain}, {PlantedKindParts{3, 0, 0, 1}}));
        assert(refuses({plain}, {PlantedKindParts{0, plantingMostKinds, 0, 1}}));
        assert(refuses(std::vector<DrawItem>(placedMostParts + 1, plain), {PlantedKindParts{0, 0, 0, placedMostParts + 1}}));
    }

    void testTheBlocks() {
        std::mt19937 random{7};
        const TestSlots slots = testSlots(random);
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(slots.held, slots.layout, table);
        std::size_t expected = 0;
        std::size_t block = 0;

        for (const auto& [cell, slot] : slots.held) {
            for (std::size_t layer = 0; layer < slots.layers; ++layer) {
                if (!table.drawnLayers[layer]) {
                    continue;
                }

                const std::uint32_t room = slots.layout.layerCapacity[layer];

                for (std::uint32_t first = 0; first < room; first += placedGroupSize, ++block) {
                    const GpuPlacedBlock& made = blocks.at(block);
                    assert(made.firstPlant == static_cast<std::uint32_t>(slot) * slots.layout.plantsASlot + slots.layout.layerFirst[layer] + first);
                    assert(made.countPlace == static_cast<std::uint32_t>(slot) * slots.layers + layer);
                    assert(made.firstInLayer == first && made.firstKind == layer * static_cast<std::uint32_t>(plantingMostKinds));
                    ++expected;
                }
            }
        }

        assert(blocks.size() == expected);
        assert(placedEntryRoom(blocks.size(), table) == blocks.size() * placedGroupSize * 2 * table.mostParts);
        const GpuPlacedView view = packPlacedView(GpuCameraCull{}, testRegion, blocks.size(), table);
        assert(view.blocks == blocks.size() && view.groups == table.groups.size() && view.entryRoom == placedEntryRoom(blocks.size(), table));
        assert(view.region[0] == testRegion.northmost && view.region[1] == testRegion.southmost && view.region[2] == testRegion.spread);
    }

    /*
    * The passes' lists against the entities they stand for: each plant held, counted and in the region, each of its
    * kind's parts placed by its Transform, culled and batched by the CPU's own functions (visibleDraws, opaqueDraws,
    * batchByMesh); the same entries by mesh and style, bar those within rounding of a choice.
    */
    void testTheListsAreTheEntitys() {
        std::mt19937 random{11};
        const PlantingMaps maps = straightMaps();
        std::size_t compared = 0;
        std::size_t nearEdges = 0;
        std::size_t fading = 0;
        std::size_t thinned = 0;

        for (int test = 0; test < 30; ++test) {
            const TestSlots slots = testSlots(random);
            const TestKinds kinds = testKinds();
            const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
            const std::vector<GpuPlacedBlock> blocks = placedBlocks(slots.held, slots.layout, table);
            const TestCamera camera = randomCamera(random, test);
            const GpuPlacedView view = packPlacedView(camera.packed, testRegion, blocks.size(), table);
            const PlacedCullResult result = cullPlacedOnCpu(table, blocks, slots.plants, slots.counts, maps, view);

            // The entities: every drawn part of every plant that counts, with its plant and part beside it.
            std::vector<DrawItem> entities;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> owners;

            for (const GpuPlacedBlock& block : blocks) {
                const std::uint32_t count = slots.counts[block.countPlace];

                for (std::uint32_t index = block.firstInLayer; index < std::min(count, block.firstInLayer + placedGroupSize); ++index) {
                    const std::uint32_t number = block.firstPlant + (index - block.firstInLayer);
                    const GpuPlacedPlant& plant = slots.plants[number];

                    if (!insidePlantedRegion(maps, testRegion, Vec3{plant.foot[0], plant.foot[1], plant.foot[2]})) {
                        continue;
                    }

                    const GpuPlacedKind& kind = table.kinds[block.firstKind + plant.kind];

                    for (const std::uint32_t part : std::span<const std::uint32_t>{kind.part, kind.parts}) {
                        entities.push_back(entityPart(kinds.parts[table.draws[part]], plant, partBox));
                        owners.emplace_back(number, part);
                    }
                }
            }

            const BatchedDraws batched = batchByMesh(entities, opaqueDraws(entities, visibleDraws(entities, camera.frustum)), 0, &camera.levels);
            std::vector<Key> expected;

            for (const DrawBatch& batch : batched.batches) {
                for (std::uint32_t at = batch.firstInstance; at < batch.firstInstance + batch.instanceCount; ++at) {
                    const auto [plant, part] = owners[batched.order[at]];
                    expected.emplace_back(batch.mesh.index, styleNumber(batch.cutOut, batch.doubleSided), plant, part, batched.marks[at]);
                }
            }

            std::vector<Key> found;

            for (std::size_t group = 0; group < table.groups.size(); ++group) {
                const GpuDrawCommand& command = result.commands[group];
                const CullGroupStyle& style = table.styles[group];
                assert(command.firstInstance >= placedInstanceBase);
                assert(command.indexCount == table.groups[group].indexCount && command.firstIndex == table.groups[group].firstIndex);

                for (std::uint32_t at = command.firstInstance - placedInstanceBase; at < command.firstInstance - placedInstanceBase + command.instanceCount; ++at) {
                    const GpuPlacedEntry& entry = result.entries.at(at);
                    found.emplace_back(style.mesh.index, styleNumber(style.cutOut, style.doubleSided), entry.plant & DRAW_LIST_INSTANCE_MASK, entry.part,
                                       entry.plant & ~DRAW_LIST_INSTANCE_MASK);
                    fading += drawListFade(entry.plant) != 0u ? 1 : 0;
                    thinned += (entry.plant & DRAW_LIST_THINNED_BIT) != 0u ? 1 : 0;
                }
            }

            // Every entry in a group: the groups' commands tile the entries from the start, in group order.
            std::uint32_t next = placedInstanceBase;

            for (const GpuDrawCommand& command : result.commands) {
                assert(command.firstInstance == next);
                next += command.instanceCount;
            }

            assert(next - placedInstanceBase == result.entries.size());
            assert(result.entries.size() <= view.entryRoom);

            // Owners near an edge may land either way.
            const auto nearEdge = [&](const Key& key) {
                for (std::size_t entity = 0; entity < owners.size(); ++entity) {
                    if (owners[entity].first == std::get<2>(key) && owners[entity].second == std::get<3>(key)) {
                        return nearACameraEdge(packCullRecord(entities[entity]), table.parts[std::get<3>(key)].levels, camera.packed);
                    }
                }

                return false;
            };

            std::sort(expected.begin(), expected.end());
            std::vector<Key> sortedFound = found;
            std::sort(sortedFound.begin(), sortedFound.end());
            std::vector<Key> differing;
            std::set_symmetric_difference(expected.begin(), expected.end(), sortedFound.begin(), sortedFound.end(), std::back_inserter(differing));

            for (const Key& key : differing) {
                assert(nearEdge(key));
                ++nearEdges;
            }

            compared += expected.size();
        }

        // The views drew plenty (about 1,200 entries), faded and thinned some, and few choices sat on an edge.
        assert(compared > 1000 && fading > 50 && thinned > 50);
        assert(nearEdges * 100 < compared);
    }

    // Within a group, entries follow the blocks' order, then the plants' within a block.
    void testTheOrder() {
        std::mt19937 random{13};
        const TestSlots slots = testSlots(random);
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(slots.held, slots.layout, table);
        std::map<std::uint32_t, std::size_t> placeOfPlant;

        for (std::size_t block = 0; block < blocks.size(); ++block) {
            for (std::uint32_t plant = 0; plant < placedGroupSize; ++plant) {
                placeOfPlant[blocks[block].firstPlant + plant] = block * placedGroupSize + plant;
            }
        }

        const GpuPlacedView view = packPlacedView(overheadCamera(), testRegion, blocks.size(), table);
        const PlacedCullResult result = cullPlacedOnCpu(table, blocks, slots.plants, slots.counts, straightMaps(), view);
        std::size_t drawn = 0;

        for (const GpuDrawCommand& command : result.commands) {
            const std::uint32_t first = command.firstInstance - placedInstanceBase;

            for (std::uint32_t at = first + 1; at < first + command.instanceCount; ++at) {
                assert(placeOfPlant.at(result.entries[at - 1].plant & DRAW_LIST_INSTANCE_MASK) < placeOfPlant.at(result.entries[at].plant & DRAW_LIST_INSTANCE_MASK));
            }

            drawn += command.instanceCount;
        }

        // Seen from above with every mesh at its finest, each plant in the region of a drawn kind draws its parts.
        assert(drawn > 50);
    }

    void testTheComparison() {
        std::mt19937 random{17};
        const TestSlots slots = testSlots(random);
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(slots.held, slots.layout, table);
        const GpuPlacedView view = packPlacedView(overheadCamera(), testRegion, blocks.size(), table);
        const PlacedCullResult cpu = cullPlacedOnCpu(table, blocks, slots.plants, slots.counts, straightMaps(), view);
        assert(cpu.entries.size() > 10);

        const PlacedComparison same = comparePlacedDraws(table, slots.plants, view, cpu, cpu.commands, cpu.entries);
        assert(same.entries == cpu.entries.size() && same.matching == cpu.entries.size() && same.missing == 0 && same.extra == 0 && same.reordered == 0 &&
               same.commandsDiffering == 0);
        assert(describePlacedComparison(same).find("agree") != std::string::npos);

        // One entry lost from the last group drawing something, its command one shorter.
        std::vector<GpuDrawCommand> commands = cpu.commands;
        std::vector<GpuPlacedEntry> entries = cpu.entries;
        std::size_t last = commands.size() - 1;

        while (commands[last].instanceCount == 0) {
            --last;
        }

        entries.erase(entries.begin() + (commands[last].firstInstance - placedInstanceBase));
        --commands[last].instanceCount;
        const PlacedComparison lost = comparePlacedDraws(table, slots.plants, view, cpu, commands, entries);
        assert(lost.missing == 1 && lost.extra == 0 && lost.matching == cpu.entries.size() - 1 && lost.commandsDiffering == 1);
        assert(describePlacedComparison(lost).find("DISAGREE") != std::string::npos);

        // Two entries of a group swapped: the same entries, in another order.
        std::vector<GpuPlacedEntry> swapped = cpu.entries;
        std::size_t group = 0;

        while (cpu.commands[group].instanceCount < 2) {
            ++group;
        }

        const std::uint32_t first = cpu.commands[group].firstInstance - placedInstanceBase;
        std::swap(swapped[first], swapped[first + 1]);
        const PlacedComparison reordered = comparePlacedDraws(table, slots.plants, view, cpu, cpu.commands, swapped);
        assert(reordered.reordered == 1 && reordered.missing == 0 && reordered.extra == 0);
    }

    // The frame carries the looks' parts while the source has looks, none without.
    void testTheFrameCarriesTheLooks() {
        World world;
        const Entity air = world.create();
        auto looks = std::make_shared<PlantingLooks>();
        looks->region = testRegion;
        ModelRenderer tree = makeMeshRenderer(MeshHandle{10});
        tree.parts.push_back(RenderPart{MeshHandle{20}, Material{}});
        tree.castsShadows = false;
        ModelRenderer card = makeMeshRenderer(MeshHandle{30});
        looks->kinds = {PlantedKindLook{.layer = 0, .kind = 1, .models = {tree, card}}, PlantedKindLook{.layer = 2, .kind = 0, .models = {card}}};
        world.add(air, PlantingSource{std::make_shared<const PlantingField>(), 3, looks});
        const Camera camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};

        const FrameDescription frame = buildFrame(world, camera, 1.0f);
        assert(frame.plantedParts.size() == 4 && frame.plantedKinds.size() == 2);
        assert(frame.plantedKinds[0].layer == 0 && frame.plantedKinds[0].kind == 1 && frame.plantedKinds[0].first == 0 && frame.plantedKinds[0].count == 3);
        assert(frame.plantedKinds[1].layer == 2 && frame.plantedKinds[1].first == 3 && frame.plantedKinds[1].count == 1);
        assert(frame.plantedParts[1].mesh == MeshHandle{20} && !frame.plantedParts[1].castsShadows && frame.plantedParts[2].castsShadows);
        assert(frame.plantedParts[0].model == Mat4::identity() && frame.plantedRegion.spread == testRegion.spread);
        assert(frame.draws.empty());

        world.get<PlantingSource>(air).looks.reset();
        const FrameDescription unlooked = buildFrame(world, camera, 1.0f);
        assert(unlooked.plantedParts.empty() && unlooked.plantedKinds.empty() && unlooked.planting);
    }
}

void testPlacedDrawing() {
    testTheModelIsTheEntitys();
    testThePartsAreTheEntitys();
    testTheRegion();
    testTheTable();
    testTheBlocks();
    testTheListsAreTheEntitys();
    testTheOrder();
    testTheComparison();
    testTheFrameCarriesTheLooks();
}
