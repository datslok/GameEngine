#include "math/frustum.h"
#include "math/transform.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/level_fade.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/shadow_culling.h"
#include "render/gpu/transparency.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/level_of_detail.h"

#include "planting_helpers.h"

#include <algorithm>
#include <array>
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

    // Every mesh in the pools, a different place and clusters each, so commands and groups name the right one; mesh 99
    // has buffers of its own.
    MeshPlacement testPlacement(MeshHandle mesh) {
        if (mesh.index == 99) {
            return MeshPlacement{0, 100, 0, 15, false};
        }

        return MeshPlacement{1000 * mesh.index, 100, 5000 * mesh.index, 3 * (mesh.index + 1), true, 40 * mesh.index, mesh.index + 2};
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

    // Leaves swaying as the forest's trees do (render/gpu/wind.h).
    DrawItem swayingLeaves() {
        DrawItem leaves = treePart(MeshHandle{20}, &leafLevels(), true, true, 0.0f, 5);
        leaves.sway = WindSway{.kind = SwayKind::Tree, .height = 18.0f, .bend = 0.1f, .flutter = 0.3f};
        return leaves;
    }

    // The shadow's stand-in, as the forest's: casting, never seen by the camera, a cut-out without levels.
    DrawItem standInPart(MeshHandle mesh) {
        DrawItem part = treePart(mesh, nullptr, true, false, 0.0f, 0);
        part.castsShadows = true;
        part.shadowOnly = true;
        return part;
    }

    /*
    * Two kinds of layer 0 (a tree of bark, swaying leaves, card and the shadow's stand-in; one of bark and card) and one
    * of layer 1 (swaying leaves only); layer 2 not drawn. Styles mixed so the parts are sorted.
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
        kind(0, 0, {treePart(MeshHandle{10}, &barkLevels(), false, false, 0.0f, 0), swayingLeaves(), treePart(MeshHandle{30}, nullptr, true, false, 0.4f, 0),
                    standInPart(MeshHandle{40})});
        kind(0, 1, {treePart(MeshHandle{10}, &barkLevels(), false, false, 0.0f, 0), treePart(MeshHandle{31}, nullptr, true, false, 0.4f, 0)});
        kind(1, 0, {swayingLeaves()});
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
        std::vector<int> fieldLayer{0, 1, 2};
    };

    // The test slots as one set of 64 m cells, its layers the field's own.
    std::vector<PlantingSetSlots> setsOf(const TestSlots& slots) {
        return {PlantingSetSlots{slots.held, slots.layout, slots.fieldLayer, PlantingSetPlaces{}, 64.0f}};
    }

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

    // Looking straight down from 60 m on the middle of cell (0, -2) through a narrow lens: its neighbours' boxes are out.
    GpuCameraCull narrowCamera() {
        LevelOfDetailSettings settings;
        settings.enabled = false;
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.5f, 1.0f, 0.1f, 2000.0f));
        const Vec3 eye{32.0f, 60.0f, -96.0f};
        const Frustum frustum = Frustum::fromClipMatrix(projection * Mat4::lookAt(eye, Vec3{32.0f, 0.0f, -96.0f}, Vec3{0.0f, 0.0f, -1.0f}), ClipDepth::OneToZero);
        return packCameraCull(frustum, cameraLevelView(eye, 1080.0f, 0.5f, settings), false, false, 0.0f);
    }

    const PlantedRegion testRegion{.northmost = -170.0f, .southmost = -70.0f, .spread = 50.0f};

    // What the test plants keep to (randomPlant): scales 0.8 to 1.2, feet unsunk on ground from -10 to 10 m.
    PlantingLayerRecord testLayer() {
        PlantingLayerRecord layer;
        layer.leastScale = 0.8f;
        layer.scaleRange = 0.4f;
        layer.footSink = 0.0f;
        return layer;
    }

    const TerrainHeightRange testGround{-10.0f, 10.0f};

    // Each run's cell box, as the renderer makes them from the ground and the layers.
    std::vector<GpuCullChunk> runChunks(const std::vector<PlacedBlockRun>& runs, const PlacedTable& table) {
        std::vector<GpuCullChunk> chunks;

        for (const PlacedBlockRun& run : runs) {
            chunks.push_back(placedCellChunk(plantingCellBox(run.cell, 64.0f), testGround, testLayer(), table.layerReach.at(run.layer)));
        }

        return chunks;
    }

    // The views over every block (boxes no view skips): each view's passes over every plant held.
    PlacedViews everyBlock(std::span<const GpuCameraCull> asked, const std::vector<PlacedBlockRun>& runs, const PlacedTable& table) {
        std::vector<GpuCullChunk> chunks(runs.size());

        for (GpuCullChunk& chunk : chunks) {
            chunk.flags = CULL_CHUNK_SEEN | CULL_CHUNK_CASTS | CULL_CHUNK_ALWAYS;
        }

        return placedViews(asked, runs, chunks, table);
    }

    // The camera alone over every block.
    PlacedViews cameraOverEveryBlock(const GpuCameraCull& camera, const std::vector<PlacedBlockRun>& runs, const PlacedTable& table) {
        const std::array<GpuCameraCull, 1> asked{camera};
        return everyBlock(asked, runs, table);
    }

    /*
    * A sun's tile over the slots (round x 0, z -100): its box 40 to 240 m across looking down at a slant, a reach
    * sometimes, a cascade or none, sometimes a smallest caster.
    */
    GpuCameraCull randomSunTile(std::mt19937& random) {
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const Vec3 aim{(unit(random) - 0.5f) * 160.0f, 0.0f, -100.0f + (unit(random) - 0.5f) * 160.0f};
        const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
        const Vec3 toward{std::cos(yaw) * 0.6f, -1.0f, std::sin(yaw) * 0.6f};
        const Vec3 place = aim - toward * 300.0f;
        const float half = 20.0f + unit(random) * 100.0f;
        ShadowTile tile;
        tile.matrix = toGpuDepthRange(Mat4::orthographic(-half, half, -half, half, 0.0f, 700.0f)) * Mat4::lookAt(place, aim, Vec3{0.0f, 1.0f, 0.0f});
        tile.lightPlace = Vec4{-toward.x, -toward.y, -toward.z, 0.0f};
        tile.reach = unit(random) < 0.5f ? BoundingSphere{} : BoundingSphere{aim, 60.0f + unit(random) * 200.0f};
        tile.smallestCaster = unit(random) < 0.7f ? 0.0f : unit(random) * 8.0f;
        tile.sunCascade = unit(random) < 0.3f ? -1 : static_cast<int>(unit(random) * 3.0f);
        return packShadowCull(tile, LevelOfDetailSettings{});
    }

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

    /*
    * Twig parts (ROADMAP phase 21 stage 3b-iii): their groups draw nothing (no indices, no clusters: the twig pass draws
    * their twigs), each names its twig set's number in the table's sets, a set shared by two parts listed once; the
    * other parts' groups are as before.
    */
    void testTwigParts() {
        TestKinds kinds = testKinds();
        const TwigSet first;
        const TwigSet second;
        kinds.parts[0].twigs = &first;
        kinds.parts[1].twigs = &second;
        kinds.parts[2].twigs = &first;
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement);
        assert(table.twigSets.size() == 2);

        for (std::size_t place = 0; place < table.parts.size(); ++place) {
            const GpuPlacedPart& part = table.parts[place];
            const DrawItem& draw = kinds.parts[table.draws[place]];

            for (std::uint32_t slot = 0; slot <= part.levels.levels; ++slot) {
                const GpuPlacedGroup& group = table.groups[part.firstGroup + slot];
                const bool meshed = slot == 0 || draw.levels[slot - 1].mesh.isValid();
                assert(draw.twigs != nullptr ? group.indexCount == 0 && group.clusterCount == 0 : !meshed || group.indexCount > 0);
            }

            if (draw.twigs != nullptr) {
                assert((part.record.flags & CULL_TWIGS) != 0u && table.twigSets.at(part.unused[0]) == draw.twigs);
            }
        }
    }

    void testTheTable() {
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement);
        assert(table.parts.size() == kinds.parts.size() && table.draws.size() == kinds.parts.size());
        assert(table.kinds.size() == 3 * static_cast<std::size_t>(plantingMostKinds));
        assert(table.drawnLayers == (std::vector<bool>{true, true, false}));
        assert(table.mostParts == 4);

        // Each layer's reach from the foot a unit of scale: the farthest any part's bounds go across, up and down (one
        // of them reaching it), what its parts are seen by and cast into, and its most-parted kind.
        assert(table.layerReach.size() == 3);

        for (std::size_t layer = 0; layer < 3; ++layer) {
            const PlacedLayerReach& reach = table.layerReach[layer];
            float across = 0.0f;
            float up = 0.0f;
            float down = 0.0f;

            for (const PlantedKindParts& kind : kinds.kinds) {
                if (static_cast<std::size_t>(kind.layer) != layer) {
                    continue;
                }

                for (std::size_t part = kind.first; part < kind.first + kind.count; ++part) {
                    const BoundingSphere& bounds = kinds.parts[part].bounds;
                    across = std::max(across, std::hypot(bounds.centre.x, bounds.centre.z) + bounds.radius);
                    up = std::max(up, bounds.centre.y + bounds.radius);
                    down = std::max(down, bounds.radius - bounds.centre.y);
                }
            }

            assert(nearlyRelative(reach.across, across, 1e-6f) && nearlyRelative(reach.up, up, 1e-6f) && nearlyRelative(reach.down, down, 1e-6f));
            assert(reach.across >= across && reach.up >= up && reach.down >= down && !reach.unknown);
        }

        assert(table.layerReach[0].seen && table.layerReach[0].casts && table.layerReach[0].mostParts == 4);
        assert(table.layerReach[1].seen && !table.layerReach[1].casts && table.layerReach[1].mostParts == 1);
        assert(!table.layerReach[2].seen && !table.layerReach[2].casts && table.layerReach[2].mostParts == 0);

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
                assert(group.clusterCount == (mesh.isValid() ? testPlacement(mesh).clusterCount : 0u));
                assert(!mesh.isValid() || group.firstCluster == testPlacement(mesh).firstCluster);
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
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
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

        // The runs: each held cell's drawn layer's blocks, one after another as the blocks are.
        const std::vector<PlacedBlockRun> runs = placedBlockRuns(setsOf(slots), table);
        std::uint32_t next = 0;

        for (const PlacedBlockRun& run : runs) {
            assert(run.firstBlock == next && run.blocks > 0 && table.drawnLayers[run.layer]);
            const auto slot = static_cast<std::uint32_t>(slots.held.at(run.cell));

            for (std::uint32_t at = run.firstBlock; at < run.firstBlock + run.blocks; ++at) {
                assert(blocks[at].countPlace == slot * slots.layers + run.layer);
            }

            next += run.blocks;
        }

        assert(next == blocks.size());
    }

    /*
    * Two sets of cells (ROADMAP e1): the second's blocks after the first's, from its own places in the shared plants and
    * counts and its slots' layout, each layer's kinds by its field's layer; a layer the set only reads has no blocks; the
    * runs name their set and the field's layer; every counted plant of both is culled.
    */
    void testTheBlocksOfTwoSets() {
        std::mt19937 random{13};
        const TestSlots first = testSlots(random);
        std::vector<PlantingLayerRecord> field = threeLayers();
        field[2].cellSet = plantingFineCells;
        const PlantingSetLayers fine = plantingSetLayers(field, plantingFineCells);
        const PlantingSlotLayout fineLayout = plantingSlotLayout(fine.layers, 32.0f, fine.written);
        const std::map<PlantingCell, int> fineHeld{{PlantingCell{-1, -5}, 1}, {PlantingCell{0, -4}, 0}};
        const PlantingSetPlaces finePlaces{.plantBase = static_cast<std::uint32_t>(first.plants.size()),
                                           .countBase = static_cast<std::uint32_t>(first.counts.size())};
        const std::vector<PlantingSetSlots> sets{PlantingSetSlots{first.held, first.layout, first.fieldLayer, PlantingSetPlaces{}, 64.0f},
                                                 PlantingSetSlots{fineHeld, fineLayout, fine.fieldLayer, finePlaces, 32.0f}};
        TestKinds kinds = testKinds();

        for (int kind = 0; kind < 4; ++kind) {
            kinds.kinds.push_back(PlantedKindParts{.layer = 2, .kind = kind, .first = kinds.parts.size(), .count = 1});
            kinds.parts.push_back(swayingLeaves());
        }

        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, first.layers, testPlacement);
        assert(table.drawnLayers[2]);

        const std::vector<GpuPlacedBlock> alone = placedBlocks(setsOf(first), table);
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(sets, table);
        assert(std::equal(alone.begin(), alone.end(), blocks.begin(), [](const GpuPlacedBlock& a, const GpuPlacedBlock& b) {
            return a.firstPlant == b.firstPlant && a.countPlace == b.countPlace && a.firstInLayer == b.firstInLayer && a.firstKind == b.firstKind;
        }));
        const std::uint32_t perCell = (fineLayout.layerCapacity[2] + placedGroupSize - 1) / placedGroupSize;
        assert(blocks.size() == alone.size() + fineHeld.size() * perCell);
        std::size_t at = alone.size();

        for (const auto& [cell, slot] : fineHeld) {
            for (std::uint32_t block = 0; block < perCell; ++block, ++at) {
                const GpuPlacedBlock& made = blocks[at];
                assert(made.firstPlant == finePlaces.plantBase + static_cast<std::uint32_t>(slot) * fineLayout.plantsASlot + block * placedGroupSize);
                assert(made.countPlace == finePlaces.countBase + static_cast<std::uint32_t>(slot) * 3u + 2u);
                assert(made.firstKind == 2u * static_cast<std::uint32_t>(plantingMostKinds));
            }
        }

        const std::vector<PlacedBlockRun> runs = placedBlockRuns(sets, table);
        std::size_t fineRuns = 0;

        for (const PlacedBlockRun& run : runs) {
            if (run.set == 1) {
                assert(fineHeld.contains(run.cell) && run.layer == 2u && run.blocks == perCell);
                ++fineRuns;
            } else {
                assert(run.set == 0 && first.held.contains(run.cell));
            }
        }

        assert(fineRuns == fineHeld.size() && runs.back().firstBlock + runs.back().blocks == blocks.size());

        // The fine set's plants after the first set's: each counted one culled with its kind's parts.
        std::vector<GpuPlacedPlant> plants = first.plants;
        std::vector<std::uint32_t> counts = first.counts;
        plants.resize(plants.size() + 2 * fineLayout.plantsASlot);
        counts.resize(counts.size() + 2 * fine.layers.size(), 0u);
        std::size_t finePlants = 0;

        for (const auto& [cell, slot] : fineHeld) {
            const std::uint32_t count = 20u + static_cast<std::uint32_t>(slot) * 7u;
            counts[finePlaces.countBase + static_cast<std::size_t>(slot) * 3 + 2] = count;

            for (std::uint32_t plant = 0; plant < count; ++plant) {
                plants[finePlaces.plantBase + static_cast<std::size_t>(slot) * fineLayout.plantsASlot + plant] =
                    randomPlant(random, plantingCellBox(cell, 32.0f));
            }

            finePlants += count;
        }

        const PlacedViews views = cameraOverEveryBlock(overheadCamera(), runs, table);
        const PlacedCullResult all = cullPlacedOnCpu(table, blocks, views.items, plants, counts, views.views.front());
        const PlacedViews firstViews = cameraOverEveryBlock(overheadCamera(), placedBlockRuns(setsOf(first), table), table);
        const PlacedCullResult firstOnly = cullPlacedOnCpu(table, alone, firstViews.items, first.plants, first.counts, firstViews.views.front());
        std::size_t fineEntries = 0;

        for (const GpuPlacedEntry& entry : all.entries) {
            fineEntries += (entry.plant & DRAW_LIST_INSTANCE_MASK) >= finePlaces.plantBase ? 1 : 0;
        }

        assert(all.entries.size() == firstOnly.entries.size() + fineEntries && fineEntries == finePlants);
    }

    /*
    * A cell's box for a layer holds every part's bounds of every plant the cells may stand in it: feet anywhere in the
    * cell on the ground's range, any scale the layer gives, any turn; flags as its parts are seen and cast; a part of
    * unknown size makes the box always taken.
    */
    void testTheCellBoxes() {
        std::mt19937 random{19};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement);
        const PlantingCell cell{-1, -2};
        const PlantingBox box = plantingCellBox(cell, 64.0f);
        const std::array<GpuCullChunk, 2> chunks{placedCellChunk(box, testGround, testLayer(), table.layerReach[0]),
                                                 placedCellChunk(box, testGround, testLayer(), table.layerReach[1])};
        assert(chunks[0].flags == (CULL_CHUNK_SEEN | CULL_CHUNK_CASTS) && chunks[1].flags == CULL_CHUNK_SEEN);

        for (int test = 0; test < 3000; ++test) {
            GpuPlacedPlant plant = randomPlant(random, box);

            // Every tenth at a corner, the largest and turned most out.
            if (test % 10 == 0) {
                plant.foot[0] = unit(random) < 0.5f ? box.west : box.east;
                plant.foot[2] = unit(random) < 0.5f ? box.north : box.south;
                plant.foot[1] = unit(random) < 0.5f ? testGround.lowest : testGround.highest;
                plant.scale = 1.2f;
            }

            for (std::uint32_t layer = 0; layer < 2; ++layer) {
                const GpuPlacedKind& kind = table.kinds[layer * plantingMostKinds + plant.kind % (layer == 0 ? 2u : 1u)];

                for (const std::uint32_t part : std::span<const std::uint32_t>{kind.part, kind.parts}) {
                    const GpuCullRecord placed = placedCullRecord(table.parts[part].record, plant);

                    for (int axis = 0; axis < 3; ++axis) {
                        assert(placed.bounds[axis] - placed.bounds[3] >= chunks[layer].minimum[axis]);
                        assert(placed.bounds[axis] + placed.bounds[3] <= chunks[layer].maximum[axis]);
                    }
                }
            }
        }

        // Not too loose: across no more than the cell plus twice the reach at the largest scale and a margin.
        assert(chunks[0].maximum[0] - chunks[0].minimum[0] < 64.0f + 2.0f * table.layerReach[0].across * 1.2f + 1.0f);

        // A part of unknown size: the box is always taken.
        TestKinds unknown = testKinds();
        unknown.parts[0].bounds.radius = -1.0f;
        const PlacedTable unknownTable = buildPlacedTable(unknown.parts, unknown.kinds, 3, testPlacement);
        assert(unknownTable.layerReach[0].unknown);
        assert((placedCellChunk(box, testGround, testLayer(), unknownTable.layerReach[0]).flags & CULL_CHUNK_ALWAYS) != 0u);
    }

    /*
    * Each view's items are the blocks of the runs whose box it may see (chunkInView), in block order; the views follow
    * one another in the items and the entries: each its room, two entries a part of its layer's most-parted kind a plant
    * (one in a shadow tile, where nothing fades); every view's count of views and items, its groups.
    */
    void testTheViewsLayout() {
        std::mt19937 random{23};
        const TestSlots slots = testSlots(random);
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
        const std::vector<PlacedBlockRun> runs = placedBlockRuns(setsOf(slots), table);
        const std::vector<GpuCullChunk> chunks = runChunks(runs, table);
        const TestCamera camera = randomCamera(random, 0);
        const std::array<GpuCameraCull, 4> asked{narrowCamera(), randomSunTile(random), camera.packed, randomSunTile(random)};
        const PlacedViews views = placedViews(asked, runs, chunks, table);
        assert(views.views.size() == asked.size());
        std::vector<std::uint32_t> layerOfBlock(blocks.size());

        for (const PlacedBlockRun& run : runs) {
            std::fill(layerOfBlock.begin() + run.firstBlock, layerOfBlock.begin() + run.firstBlock + run.blocks, run.layer);
        }

        std::uint32_t nextItem = 0;
        std::uint32_t nextEntry = 0;

        for (std::size_t index = 0; index < views.views.size(); ++index) {
            const GpuPlacedView& view = views.views[index];
            const bool shadow = (asked[index].flags & CAMERA_CULL_SHADOW) != 0u;
            assert(view.firstItem == nextItem && view.entryBase == nextEntry);
            assert(view.viewCount == views.views.size() && view.itemCount == views.items.size() && view.groups == table.groups.size());
            assert(view.camera.flags == asked[index].flags);
            std::uint32_t room = 0;
            std::vector<std::uint32_t> expected;

            for (std::size_t run = 0; run < runs.size(); ++run) {
                if (chunkInView(chunks[run], asked[index])) {
                    for (std::uint32_t block = runs[run].firstBlock; block < runs[run].firstBlock + runs[run].blocks; ++block) {
                        expected.push_back(block);
                    }
                }
            }

            assert(view.items == expected.size());

            for (std::uint32_t item = 0; item < view.items; ++item) {
                const GpuPlacedItem& made = views.items[view.firstItem + item];
                assert(made.block == expected[item] && made.view == index);
                room += placedGroupSize * table.layerReach[layerOfBlock[made.block]].mostParts * (shadow ? 1u : 2u);
            }

            assert(view.entryRoom == room);
            nextItem += view.items;
            nextEntry += view.entryRoom;

            // Its commands count from its own entries.
            const PlacedCullResult result = cullPlacedOnCpu(table, blocks, views.items, slots.plants, slots.counts, view);
            assert(result.entries.size() <= view.entryRoom && result.commands.size() == table.groups.size());
            assert(result.commands.front().firstInstance == placedInstanceBase + view.entryBase);
        }

        assert(nextItem == views.items.size() && views.entries == nextEntry);

        // The narrow camera sees one cell's boxes of the four: the others' blocks are dropped.
        const std::size_t oneCell = std::count_if(runs.begin(), runs.end(), [](const PlacedBlockRun& run) {
            return run.cell == PlantingCell{0, -2};
        });
        assert(oneCell > 0 && views.views[0].items > 0 && views.views[0].items < blocks.size());

        for (std::uint32_t item = 0; item < views.views[0].items; ++item) {
            const std::uint32_t block = views.items[item].block;
            const auto run = std::find_if(runs.begin(), runs.end(), [&](const PlacedBlockRun& each) {
                return block >= each.firstBlock && block < each.firstBlock + each.blocks;
            });
            assert(run->cell == (PlantingCell{0, -2}));
        }
    }

    /*
    * Culling by the cells' boxes first drops nothing: every view's lists over its own items equal its lists over every
    * block (the camera and sun tiles), entry for entry; tiles take only casters.
    */
    void testTheBoxesHideNothing() {
        std::mt19937 random{29};
        std::size_t skipped = 0;
        std::size_t shadowEntries = 0;

        for (int test = 0; test < 30; ++test) {
            const TestSlots slots = testSlots(random);
            const TestKinds kinds = testKinds();
            const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
            const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
            const std::vector<PlacedBlockRun> runs = placedBlockRuns(setsOf(slots), table);
            const GpuCameraCull camera = test % 3 == 0 ? overheadCamera() : randomCamera(random, test).packed;
            const std::array<GpuCameraCull, 3> asked{camera, randomSunTile(random), randomSunTile(random)};
            const PlacedViews culled = placedViews(asked, runs, runChunks(runs, table), table);
            const PlacedViews every = everyBlock(asked, runs, table);
            std::vector<PlacedCullResult> results;

            for (std::size_t index = 0; index < asked.size(); ++index) {
                const PlacedCullResult mine = cullPlacedOnCpu(table, blocks, culled.items, slots.plants, slots.counts, culled.views[index]);
                const PlacedCullResult all = cullPlacedOnCpu(table, blocks, every.items, slots.plants, slots.counts, every.views[index]);
                assert(mine.entries == all.entries);

                for (std::size_t group = 0; group < table.groups.size(); ++group) {
                    assert(mine.commands[group].instanceCount == all.commands[group].instanceCount);
                    assert(mine.commands[group].firstInstance - culled.views[index].entryBase == all.commands[group].firstInstance - every.views[index].entryBase);
                }

                skipped += blocks.size() - culled.views[index].items;
                results.push_back(mine);
            }

            // The tiles' entries are casters (the stand-in alone).
            for (std::size_t tile = 1; tile < 3; ++tile) {
                for (const GpuPlacedEntry& entry : results[tile].entries) {
                    assert((table.parts[entry.part].record.flags & CULL_CASTS_SHADOWS) != 0u);
                }

                shadowEntries += results[tile].entries.size();
            }

        }

        assert(skipped > 1000 && shadowEntries > 100);
    }

    /*
    * A cell made or dropped stands for its boxes in the shadow tiles' tests: one casting sphere holding every casting
    * layer's box (none casting: it touches no tile). The table's signature changes with its parts, kinds or region.
    */
    void testTheChanges() {
        const TestKinds kinds = testKinds();
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement);
        const PlantingBox box = plantingCellBox(PlantingCell{2, -3}, 64.0f);
        const std::array<GpuCullChunk, 2> chunks{placedCellChunk(box, testGround, testLayer(), table.layerReach[0]),
                                                 placedCellChunk(box, testGround, testLayer(), table.layerReach[1])};
        const GpuCullRecord record = placedChangeRecord(chunks);
        assert((record.flags & CULL_CASTS_SHADOWS) != 0u && (record.flags & (CULL_HIDDEN | CULL_TRANSPARENT)) == 0u && record.shadowCascades == 0);
        assert(record.bounds[3] > 0.0f && record.mesh != cullNoMesh);

        for (int corner = 0; corner < 8; ++corner) {
            const GpuCullChunk& chunk = chunks[0];
            const float x = (corner & 1) != 0 ? chunk.maximum[0] : chunk.minimum[0];
            const float y = (corner & 2) != 0 ? chunk.maximum[1] : chunk.minimum[1];
            const float z = (corner & 4) != 0 ? chunk.maximum[2] : chunk.minimum[2];
            const float dx = x - record.bounds[0];
            const float dy = y - record.bounds[1];
            const float dz = z - record.bounds[2];
            assert(std::sqrt(dx * dx + dy * dy + dz * dz) <= record.bounds[3]);
        }

        const std::array<GpuCullChunk, 1> seenOnly{chunks[1]};
        assert((placedChangeRecord(seenOnly).flags & CULL_CASTS_SHADOWS) == 0u);

        const std::uint64_t signature = placedTableSignature(table);
        assert(placedTableSignature(buildPlacedTable(kinds.parts, kinds.kinds, 3, testPlacement)) == signature);
        TestKinds fewer = testKinds();
        fewer.kinds.pop_back();
        assert(placedTableSignature(buildPlacedTable(fewer.parts, fewer.kinds, 3, testPlacement)) != signature);
        TestKinds casting = testKinds();
        casting.parts[0].castsShadows = true;
        assert(placedTableSignature(buildPlacedTable(casting.parts, casting.kinds, 3, testPlacement)) != signature);
    }

    /*
    * The passes' lists against the entities they stand for: each plant held and counted, each of its
    * kind's parts placed by its Transform, culled and batched by the CPU's own functions (visibleDraws, opaqueDraws,
    * batchByMesh); the same entries by mesh and style, bar those within rounding of a choice.
    */
    void testTheListsAreTheEntitys() {
        std::mt19937 random{11};
        std::size_t compared = 0;
        std::size_t nearEdges = 0;
        std::size_t fading = 0;
        std::size_t thinned = 0;

        for (int test = 0; test < 30; ++test) {
            const TestSlots slots = testSlots(random);
            const TestKinds kinds = testKinds();
            const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
            const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
            const TestCamera camera = randomCamera(random, test);
            const PlacedViews views = cameraOverEveryBlock(camera.packed, placedBlockRuns(setsOf(slots), table), table);
            const GpuPlacedView& view = views.views.front();
            const PlacedCullResult result = cullPlacedOnCpu(table, blocks, views.items, slots.plants, slots.counts, view);

            // The entities: every drawn part of every plant that counts, with its plant and part beside it.
            std::vector<DrawItem> entities;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> owners;

            for (const GpuPlacedBlock& block : blocks) {
                const std::uint32_t count = slots.counts[block.countPlace];

                for (std::uint32_t index = block.firstInLayer; index < std::min(count, block.firstInLayer + placedGroupSize); ++index) {
                    const std::uint32_t number = block.firstPlant + (index - block.firstInLayer);
                    const GpuPlacedPlant& plant = slots.plants[number];
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
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
        std::map<std::uint32_t, std::size_t> placeOfPlant;

        for (std::size_t block = 0; block < blocks.size(); ++block) {
            for (std::uint32_t plant = 0; plant < placedGroupSize; ++plant) {
                placeOfPlant[blocks[block].firstPlant + plant] = block * placedGroupSize + plant;
            }
        }

        const PlacedViews views = cameraOverEveryBlock(overheadCamera(), placedBlockRuns(setsOf(slots), table), table);
        const GpuPlacedView& view = views.views.front();
        const PlacedCullResult result = cullPlacedOnCpu(table, blocks, views.items, slots.plants, slots.counts, view);
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
        const std::vector<GpuPlacedBlock> blocks = placedBlocks(setsOf(slots), table);
        const PlacedViews views = cameraOverEveryBlock(overheadCamera(), placedBlockRuns(setsOf(slots), table), table);
        const GpuPlacedView& view = views.views.front();
        const PlacedCullResult cpu = cullPlacedOnCpu(table, blocks, views.items, slots.plants, slots.counts, view);
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
        assert(frame.plantedParts[0].model == Mat4::identity());
        assert(frame.draws.empty());

        world.get<PlantingSource>(air).looks.reset();
        const FrameDescription unlooked = buildFrame(world, camera, 1.0f);
        assert(unlooked.plantedParts.empty() && unlooked.plantedKinds.empty() && unlooked.planting);
    }
}

namespace {
    /*
    * A kind given several looks (the understorey's scans, several plants a file) draws the one its plant's pick chooses,
    * min(pick * variants, variants - 1) as the entities chose; its variants past the first take the layer's top kind
    * slots, never one a look's kind holds.
    */
    void testTheVariants() {
        assert(placedKindSlot(2, 0, 0, 0.7f) == 2 && placedKindSlot(2, 1, 0, 0.7f) == 2);
        assert(placedKindSlot(2, 4, 29, 0.0f) == 2 && placedKindSlot(2, 4, 29, 0.25f) == 29 && placedKindSlot(2, 4, 29, 0.9999f) == 31);
        assert(placedKindSlot(2, 4, 29, 1.0f) == 31);

        for (int each = 0; each < 1000; ++each) {
            const float pick = static_cast<float>(each) / 999.0f;
            const std::uint32_t chosen = std::min(static_cast<std::uint32_t>(pick * 3.0f), 2u);
            assert(placedKindSlot(1, 3, 30, pick) == (chosen == 0 ? 1u : 30u + chosen - 1u));
        }

        TestKinds test;
        const auto look = [&](int layer, int which, int variant, MeshHandle mesh) {
            test.kinds.push_back(PlantedKindParts{.layer = layer, .kind = which, .first = test.parts.size(), .count = 1, .variant = variant});
            test.parts.push_back(treePart(mesh, nullptr, false, false, 0.0f, 0));
        };
        look(1, 0, 0, MeshHandle{10});
        look(1, 0, 1, MeshHandle{11});
        look(1, 0, 2, MeshHandle{12});
        look(1, 1, 0, MeshHandle{20});
        look(0, 0, 0, MeshHandle{30});
        const PlacedTable table = buildPlacedTable(test.parts, test.kinds, 3, testPlacement);
        const std::size_t layerOne = plantingMostKinds;
        const GpuPlacedKind& ferns = table.kinds[layerOne];
        assert(ferns.variants == 3 && ferns.variantSlot == static_cast<std::uint32_t>(plantingMostKinds) - 2);
        assert(table.kinds[layerOne + 1].variants == 1 && table.kinds[0].variants == 1);

        // Each pick's slot holds that variant's part.
        for (const auto& [pick, mesh] : {std::pair{0.1f, 10u}, std::pair{0.5f, 11u}, std::pair{0.9f, 12u}}) {
            const std::uint32_t slot = placedKindSlot(0, ferns.variants, ferns.variantSlot, pick);
            const GpuPlacedKind& chosen = table.kinds[layerOne + slot];
            assert(chosen.parts == 1 && table.parts[chosen.part[0]].record.mesh == mesh);
        }

        // Refused: a variant missing below another, and variants reaching a slot a look's kind holds.
        const auto refuses = [&](std::vector<PlantedKindParts> runs) {
            try {
                buildPlacedTable(test.parts, runs, 3, testPlacement);
            } catch (const std::invalid_argument&) {
                return true;
            }

            return false;
        };
        assert(refuses({PlantedKindParts{.layer = 1, .kind = 0, .first = 0, .count = 1, .variant = 0},
                        PlantedKindParts{.layer = 1, .kind = 0, .first = 1, .count = 1, .variant = 2}}));
        std::vector<PlantedKindParts> crowded{PlantedKindParts{.layer = 1, .kind = 31, .first = 0, .count = 1, .variant = 0},
                                              PlantedKindParts{.layer = 1, .kind = 0, .first = 1, .count = 1, .variant = 0},
                                              PlantedKindParts{.layer = 1, .kind = 0, .first = 2, .count = 1, .variant = 1}};
        assert(refuses(crowded));
        crowded.front().kind = 30;
        assert(!refuses(crowded));
    }
}

namespace {
    /*
    * How each layer is drawn (ROADMAP d3): a layer's ground-following parts' culling spheres grown by its relief, the rest
    * not; in the table's signature; the frame carries the looks' layers.
    */
    void testTheLayersDrawing() {
        std::mt19937 random{29};
        const TestSlots slots = testSlots(random);
        TestKinds kinds = testKinds();
        std::vector<int> layerOfPart(kinds.parts.size(), -1);

        for (const PlantedKindParts& kind : kinds.kinds) {
            for (std::size_t part = kind.first; part < kind.first + kind.count; ++part) {
                layerOfPart[part] = kind.layer;
            }
        }

        const std::size_t following = static_cast<std::size_t>(std::find(layerOfPart.begin(), layerOfPart.end(), 0) - layerOfPart.begin());
        kinds.parts[following].followsGround = true;
        const std::vector<PlantedLayerDrawing> layers{PlantedLayerDrawing{.groundRelief = 2.5f}, PlantedLayerDrawing{.groundRelief = 7.0f}};
        const PlacedTable plain = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement);
        const PlacedTable table = buildPlacedTable(kinds.parts, kinds.kinds, slots.layers, testPlacement, layers);

        for (std::size_t place = 0; place < table.parts.size(); ++place) {
            const std::size_t part = table.draws[place];
            const float grown = part == following ? 2.5f : 0.0f;
            assert(table.parts[place].record.bounds[3] == plain.parts[place].record.bounds[3] + grown);
        }

        assert(placedTableSignature(table) != placedTableSignature(plain));

        World world;
        auto looks = std::make_shared<PlantingLooks>();
        looks->layers = layers;
        world.add(world.create(), PlantingSource{std::make_shared<const PlantingField>(), 1, looks});
        const Camera camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
        const FrameDescription frame = buildFrame(world, camera, 1.0f);
        assert(frame.plantedLayers.size() == 2 && frame.plantedLayers[1].groundRelief == 7.0f && frame.plantedLayers[0].groundRelief == 2.5f);
    }
}

void testPlacedDrawing() {
    testTheLayersDrawing();
    testTheModelIsTheEntitys();
    testThePartsAreTheEntitys();
    testTheRegion();
    testTheTable();
    testTwigParts();
    testTheVariants();
    testTheBlocks();
    testTheBlocksOfTwoSets();
    testTheCellBoxes();
    testTheViewsLayout();
    testTheBoxesHideNothing();
    testTheChanges();
    testTheListsAreTheEntitys();
    testTheOrder();
    testTheComparison();
    testTheFrameCarriesTheLooks();
}
