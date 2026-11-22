#include "render/gpu/placed_drawing.h"
#include "render/gpu/column_major.h"
#include "render/gpu/transparency.h"
#include "scene/material.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
    // batchByMesh's ways of drawing: plain, double-sided, cut-out, double-sided cut-out.
    int partStyle(const Material& material) {
        return (isCutOut(material) ? 2 : 0) + (material.doubleSided ? 1 : 0);
    }

    // The turn's diagonal (1 - 2 sin^2) and corner (2 cos sin) of half the angle, as Quat::fromAxisAngle about up and
    // Quat::toMatrix make them (x and z are 0 there).
    struct PlacedTurn {
        float diagonal = 1.0f;
        float corner = 0.0f;
    };

    PlacedTurn placedTurn(float turnRadians) {
        const float halfAngle = turnRadians * 0.5f;
        const float sine = std::sin(halfAngle);
        const float cosine = std::cos(halfAngle);
        return PlacedTurn{1.0f - 2.0f * (sine * sine + 0.0f), 2.0f * (0.0f + cosine * sine)};
    }

    // Transform::getMatrix's steps with the quaternion's zero parts left out where they change nothing, from the turn
    // already made (placed_drawing.glsl's placedTurnedModel: a vertex makes the sine and cosine once).
    Mat4 placedTurnedModel(const GpuPlacedPlant& plant, const PlacedTurn& turn) {
        Mat4 model = Mat4::identity();
        model.values[0][0] = turn.diagonal * plant.scale;
        model.values[0][2] = turn.corner * plant.scale;
        model.values[1][1] = plant.scale;
        model.values[2][0] = -turn.corner * plant.scale;
        model.values[2][2] = turn.diagonal * plant.scale;
        model.values[0][3] = plant.foot[0];
        model.values[1][3] = plant.foot[1];
        model.values[2][3] = plant.foot[2];
        return model;
    }

    // A sphere placed by the plant's matrix already made (placedSphereBy), its radius by the plant's scale.
    std::array<float, 4> placedSphereBy(const float sphere[4], const Mat4& model, float scale) {
        const float x = model.values[0][0] * sphere[0] + model.values[0][2] * sphere[2] + model.values[0][3];
        const float y = model.values[1][1] * sphere[1] + model.values[1][3];
        const float z = model.values[2][0] * sphere[0] + model.values[2][2] * sphere[2] + model.values[2][3];
        return {x, y, z, sphere[3] < 0.0f ? sphere[3] : sphere[3] * scale};
    }

    // The part's draws as the passes make them (placedPartEntries): up to two slots, each a group and an entry.
    struct PartDraws {
        std::uint32_t groups[2] = {placedMostGroups, placedMostGroups};
        std::uint32_t entries[2] = {0, 0};
    };

    PartDraws placedPartEntries(const GpuPlacedPart& part, const GpuPlacedPlant& plant, std::uint32_t number, const GpuCameraCull& view) {
        const GpuCullRecord record = placedCullRecord(part.record, plant);
        PartDraws draws;
        std::uint32_t found = 0;

        for (std::uint32_t slot = 0; slot <= part.levels.levels && slot < CULL_LEVEL_SLOTS; ++slot) {
            if (part.levels.meshes[slot] == cullNoMesh) {
                continue;
            }

            const std::uint32_t flags = cameraCandidateFlags(record, part.levels, view, slot);

            if ((flags & CAMERA_CULL_DRAWN) == 0u || found >= 2) {
                continue;
            }

            const std::uint32_t fade = (flags & CAMERA_CULL_FADES) != 0u ? cameraCandidateFade(record, part.levels, view, slot) : 0u;
            draws.groups[found] = part.firstGroup + slot;
            draws.entries[found] = cameraCandidateEntry(number, flags, fade);
            ++found;
        }

        return draws;
    }

    // Whether a block's thread has a plant to cull (placedBlockPlant): within its slot's count.
    bool blockPlant(const GpuPlacedBlock& block, std::uint32_t thread, std::span<const std::uint32_t> counts) {
        return block.firstInLayer + thread < counts[block.countPlace];
    }

    // A part's bounds into its layer's reach: across, up and down from the foot, what sees it and what it casts into.
    void reachFor(PlacedLayerReach& reach, const DrawItem& draw) {
        const GpuCullRecord record = packCullRecord(draw);
        reach.seen = reach.seen || (record.flags & (CULL_SHADOW_ONLY | CULL_HIDDEN)) == 0u;
        reach.casts = reach.casts || ((record.flags & CULL_CASTS_SHADOWS) != 0u && (record.flags & CULL_HIDDEN) == 0u);

        if (record.bounds[3] < 0.0f) {
            reach.unknown = true;
            return;
        }

        reach.across = std::max(reach.across, std::hypot(record.bounds[0], record.bounds[2]) + record.bounds[3]);
        reach.up = std::max(reach.up, record.bounds[1] + record.bounds[3]);
        reach.down = std::max(reach.down, record.bounds[3] - record.bounds[1]);
    }

    // FNV-1a over a span's bytes, carried on from hash.
    template<typename Element>
    std::uint64_t mixSpan(std::uint64_t hash, std::span<const Element> elements) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(elements.data());

        for (std::size_t at = 0; at < elements.size_bytes(); ++at) {
            hash = (hash ^ bytes[at]) * 1099511628211ull;
        }

        return hash;
    }
}

/*
* Parts sorted by style (stable, so a kind's parts keep their order within a style), each given its slots' groups in
* turn, so the groups too run style by style; the kinds name their parts' new places. Each style's run is one
* multi-draw (the drawing order is the groups' own).
*/
PlacedTable buildPlacedTable(std::span<const DrawItem> parts, std::span<const PlantedKindParts> kinds, std::size_t layerCount,
                             const std::function<MeshPlacement(MeshHandle)>& placement, std::span<const PlantedLayerDrawing> layers) {
    PlacedTable table;
    table.kinds.resize(layerCount * static_cast<std::size_t>(plantingMostKinds));
    table.drawnLayers.assign(layerCount, false);
    table.layerReach.assign(layerCount, PlacedLayerReach{});
    std::vector<std::size_t> order;
    std::vector<float> reliefOf(parts.size(), 0.0f);
    const auto drawingOf = [&](int layer) {
        return static_cast<std::size_t>(layer) < layers.size() ? layers[static_cast<std::size_t>(layer)] : PlantedLayerDrawing{};
    };

    for (const PlantedKindParts& kind : kinds) {
        if (kind.layer < 0 || static_cast<std::size_t>(kind.layer) >= layerCount || kind.kind < 0 || kind.kind >= plantingMostKinds) {
            throw std::invalid_argument("A placed kind's layer or kind is past the field's");
        }

        if (kind.count > placedMostParts) {
            throw std::invalid_argument("A placed kind has more parts than the passes take");
        }

        for (std::size_t part = kind.first; part < kind.first + kind.count; ++part) {
            const DrawItem& draw = parts[part];

            if (isTransparent(draw.material) || !draw.mesh.isValid()) {
                throw std::invalid_argument("A placed part is glass or has no mesh");
            }

            if (draw.levels.size() >= meshLevelSlots) {
                throw std::invalid_argument("A mesh has at most three levels besides itself");
            }

            order.push_back(part);
            reachFor(table.layerReach[static_cast<std::size_t>(kind.layer)], draw);
            reliefOf[part] = draw.followsGround ? drawingOf(kind.layer).groundRelief : 0.0f;
        }

        PlacedLayerReach& reach = table.layerReach[static_cast<std::size_t>(kind.layer)];
        reach.mostParts = std::max(reach.mostParts, static_cast<std::uint32_t>(kind.count));
        table.drawnLayers[static_cast<std::size_t>(kind.layer)] = table.drawnLayers[static_cast<std::size_t>(kind.layer)] || kind.count > 0;
        table.mostParts = std::max(table.mostParts, static_cast<std::uint32_t>(kind.count));
    }

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return partStyle(parts[a].material) < partStyle(parts[b].material);
    });
    std::vector<std::uint32_t> placeOf(parts.size(), 0);

    for (const std::size_t part : order) {
        const DrawItem& draw = parts[part];
        GpuPlacedPart placed;
        placed.record = packCullRecord(draw);

        // A ground-following part's vertices lie up to its layer's relief above or below where its plant stands.
        if (placed.record.bounds[3] >= 0.0f) {
            placed.record.bounds[3] += reliefOf[part];
        }

        placed.levels = packMeshLevels(draw.mesh, draw.levels);
        placed.firstGroup = static_cast<std::uint32_t>(table.groups.size());
        const bool cutOut = isCutOut(draw.material);

        for (std::uint32_t slot = 0; slot <= placed.levels.levels; ++slot) {
            const MeshHandle mesh = slot == 0 ? draw.mesh : draw.levels[slot - 1].mesh;
            GpuPlacedGroup group;

            if (mesh.isValid()) {
                const MeshPlacement where = placement(mesh);

                if (!where.pooled) {
                    throw std::invalid_argument("A placed part's mesh is not in the pools");
                }

                group.indexCount = where.indexCount;
                group.firstIndex = where.firstIndex;
                group.vertexOffset = static_cast<std::int32_t>(where.firstVertex);
                group.firstCluster = where.firstCluster;
                group.clusterCount = where.clusterCount;
            }

            table.groups.push_back(group);
            table.styles.push_back(CullGroupStyle{mesh, cutOut, draw.material.doubleSided, true});
        }

        placeOf[part] = static_cast<std::uint32_t>(table.parts.size());
        table.parts.push_back(placed);
        table.draws.push_back(part);
    }

    if (table.groups.size() > placedMostGroups) {
        throw std::invalid_argument("The placed parts have more level slots than the passes take");
    }

    /*
    * Each kind's variants (its looks, 0 to n - 1, each once) and where those past the first go: down from the layer's
    * top slot, kind by kind in order, each kind's run upwards; every slot below the lowest stays its own kind's.
    */
    std::map<std::pair<int, int>, std::vector<int>> variantsOf;
    std::vector<int> highestKind(layerCount, -1);

    for (const PlantedKindParts& kind : kinds) {
        variantsOf[{kind.layer, kind.kind}].push_back(kind.variant);
        highestKind[static_cast<std::size_t>(kind.layer)] = std::max(highestKind[static_cast<std::size_t>(kind.layer)], kind.kind);
    }

    std::map<std::pair<int, int>, std::uint32_t> firstVariantSlot;
    std::vector<int> lowestVariantSlot(layerCount, plantingMostKinds);

    for (auto& [kind, variants] : variantsOf) {
        std::sort(variants.begin(), variants.end());

        for (std::size_t each = 0; each < variants.size(); ++each) {
            if (variants[each] != static_cast<int>(each)) {
                throw std::invalid_argument("A placed kind's variants must be 0 to n - 1, each once");
            }
        }

        int& lowest = lowestVariantSlot[static_cast<std::size_t>(kind.first)];
        lowest -= static_cast<int>(variants.size()) - 1;
        firstVariantSlot[kind] = static_cast<std::uint32_t>(lowest);

        if (lowest <= highestKind[static_cast<std::size_t>(kind.first)]) {
            throw std::invalid_argument("A layer's kinds and their variants past the first need more than its kind slots");
        }
    }

    // Each kind names its parts' places, in its own order (the parts run by style, so a kind's are not one run).
    for (const PlantedKindParts& kind : kinds) {
        const std::pair<int, int> key{kind.layer, kind.kind};
        const std::uint32_t variantSlot = firstVariantSlot.at(key);
        const std::uint32_t slot = kind.variant == 0 ? static_cast<std::uint32_t>(kind.kind) : variantSlot + static_cast<std::uint32_t>(kind.variant) - 1;
        GpuPlacedKind& placed = table.kinds[static_cast<std::size_t>(kind.layer * plantingMostKinds) + slot];
        placed.parts = static_cast<std::uint32_t>(kind.count);

        if (kind.variant == 0) {
            placed.variants = static_cast<std::uint32_t>(variantsOf.at(key).size());
            placed.variantSlot = placed.variants > 1 ? variantSlot : 0u;
        }

        for (std::size_t part = 0; part < kind.count; ++part) {
            placed.part[part] = placeOf[kind.first + part];
        }
    }

    table.order.order.resize(table.groups.size());

    for (std::size_t group = 0; group < table.groups.size(); ++group) {
        const CullGroupStyle& style = table.styles[group];
        const std::size_t run = static_cast<std::size_t>((style.cutOut ? 2 : 0) + (style.doubleSided ? 1 : 0));
        table.order.order[group] = static_cast<std::uint32_t>(group);

        if (table.order.runCount[run] == 0) {
            table.order.runFirst[run] = static_cast<std::uint32_t>(group);
        }

        ++table.order.runCount[run];
    }

    table.order.pooled = static_cast<std::uint32_t>(table.groups.size());
    return table;
}

namespace {
    // Each held cell's drawn layers of every set in placedBlocks' order: its set, the set's slots, cell and slot, the
    // set's layer and the field's, and that layer's blocks.
    template<typename Visit>
    void forEachDrawnLayer(std::span<const PlantingSetSlots> sets, const PlacedTable& table, Visit&& visit) {
        for (std::size_t set = 0; set < sets.size(); ++set) {
            const PlantingSetSlots& slots = sets[set];
            const auto layers = static_cast<std::uint32_t>(slots.layout.layerFirst.size());

            for (const auto& [cell, slot] : slots.held) {
                for (std::uint32_t layer = 0; layer < layers; ++layer) {
                    const auto field = static_cast<std::uint32_t>(slots.fieldLayer.at(layer));

                    if (field >= table.drawnLayers.size() || !table.drawnLayers[field]) {
                        continue;
                    }

                    const std::uint32_t blocks = (slots.layout.layerCapacity[layer] + placedGroupSize - 1) / placedGroupSize;

                    if (blocks > 0) {
                        visit(static_cast<int>(set), slots, cell, static_cast<std::uint32_t>(slot), layer, field, blocks);
                    }
                }
            }
        }
    }
}

std::vector<GpuPlacedBlock> placedBlocks(std::span<const PlantingSetSlots> sets, const PlacedTable& table) {
    std::vector<GpuPlacedBlock> blocks;
    forEachDrawnLayer(sets, table, [&](int, const PlantingSetSlots& slots, const PlantingCell&, std::uint32_t slot, std::uint32_t layer, std::uint32_t field,
                                       std::uint32_t count) {
        const auto layers = static_cast<std::uint32_t>(slots.layout.layerFirst.size());

        for (std::uint32_t block = 0; block < count; ++block) {
            const std::uint32_t first = block * placedGroupSize;
            blocks.push_back(GpuPlacedBlock{slots.places.plantBase + slot * slots.layout.plantsASlot + slots.layout.layerFirst[layer] + first,
                                            slots.places.countBase + slot * layers + layer, first, field * static_cast<std::uint32_t>(plantingMostKinds)});
        }
    });
    return blocks;
}

// placedBlocks' walk, counting each held cell's drawn layer's blocks instead of making them.
std::vector<PlacedBlockRun> placedBlockRuns(std::span<const PlantingSetSlots> sets, const PlacedTable& table) {
    std::vector<PlacedBlockRun> runs;
    std::uint32_t next = 0;
    forEachDrawnLayer(sets, table, [&](int set, const PlantingSetSlots&, const PlantingCell& cell, std::uint32_t, std::uint32_t, std::uint32_t field,
                                       std::uint32_t count) {
        runs.push_back(PlacedBlockRun{set, cell, field, next, count});
        next += count;
    });
    return runs;
}

/*
* Feet anywhere in the cell, at most the layer's largest scale (plantingScale's top), on the ground less the sink; the
* turn moves a part round the foot, so its centre's distance across and its radius bound it every way across.
*/
GpuCullChunk placedCellChunk(const PlantingBox& cell, const TerrainHeightRange& ground, const PlantingLayerRecord& layer, const PlacedLayerReach& reach) {
    GpuCullChunk chunk{};
    chunk.flags = (reach.seen ? CULL_CHUNK_SEEN : 0u) | (reach.casts ? CULL_CHUNK_CASTS : 0u) | (reach.unknown ? CULL_CHUNK_ALWAYS : 0u);
    const float scale = layer.leastScale + layer.scaleRange;
    const float across = reach.across * scale;
    const float low[3] = {std::min(cell.west, cell.east) - across, ground.lowest - layer.footSink - reach.down * scale, std::min(cell.north, cell.south) - across};
    const float high[3] = {std::max(cell.west, cell.east) + across, ground.highest - layer.footSink + reach.up * scale, std::max(cell.north, cell.south) + across};
    setCullChunkBox(chunk, low, high);
    return chunk;
}

/*
* A view's items in run order, so its blocks keep placedBlocks' order (the entries' order within a group); its room by
* its own items' layers. A shadow tile's parts never fade, so one entry a part there.
*/
PlacedViews placedViews(std::span<const GpuCameraCull> asked, std::span<const PlacedBlockRun> runs, std::span<const GpuCullChunk> chunks,
                        const PlacedTable& table) {
    PlacedViews made;

    for (std::size_t index = 0; index < asked.size(); ++index) {
        const GpuCameraCull& camera = asked[index];
        const std::uint32_t entriesAPart = (camera.flags & CAMERA_CULL_SHADOW) != 0u ? 1u : 2u;
        GpuPlacedView view;
        view.camera = camera;
        view.firstItem = static_cast<std::uint32_t>(made.items.size());
        view.groups = static_cast<std::uint32_t>(table.groups.size());
        view.entryBase = made.entries;

        for (std::size_t run = 0; run < runs.size(); ++run) {
            if (!chunkInView(chunks[run], camera)) {
                continue;
            }

            for (std::uint32_t block = runs[run].firstBlock; block < runs[run].firstBlock + runs[run].blocks; ++block) {
                made.items.push_back(GpuPlacedItem{block, static_cast<std::uint32_t>(index)});
            }

            view.entryRoom += runs[run].blocks * placedGroupSize * table.layerReach.at(runs[run].layer).mostParts * entriesAPart;
        }

        view.items = static_cast<std::uint32_t>(made.items.size()) - view.firstItem;
        made.entries += view.entryRoom;
        made.views.push_back(view);
    }

    for (GpuPlacedView& view : made.views) {
        view.viewCount = static_cast<std::uint32_t>(made.views.size());
        view.itemCount = static_cast<std::uint32_t>(made.items.size());
    }

    return made;
}

// The casting boxes' corners' box, its centre and half its diagonal.
GpuCullRecord placedChangeRecord(std::span<const GpuCullChunk> chunks) {
    GpuCullRecord record{};
    record.mesh = 0;
    record.bounds[3] = 0.0f;
    float low[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float high[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    bool casts = false;

    for (const GpuCullChunk& chunk : chunks) {
        if ((chunk.flags & CULL_CHUNK_CASTS) == 0u) {
            continue;
        }

        casts = true;

        if ((chunk.flags & CULL_CHUNK_ALWAYS) != 0u) {
            record.bounds[3] = -1.0f;
        }

        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], chunk.minimum[axis]);
            high[axis] = std::max(high[axis], chunk.maximum[axis]);
        }
    }

    if (!casts) {
        return record;
    }

    record.flags = CULL_CASTS_SHADOWS;
    float squared = 0.0f;

    for (int axis = 0; axis < 3; ++axis) {
        record.bounds[axis] = 0.5f * (low[axis] + high[axis]);
        const float half = 0.5f * (high[axis] - low[axis]);
        squared += half * half;
    }

    // Grown a hair past rounding, so the corners lie inside.
    if (record.bounds[3] >= 0.0f) {
        record.bounds[3] = std::sqrt(squared) * (1.0f + 1e-5f) + static_cast<float>(CULL_CHUNK_MARGIN);
    }

    std::copy(record.bounds, record.bounds + 4, record.levelSphere);
    return record;
}

std::uint64_t placedTableSignature(const PlacedTable& table) {
    std::uint64_t hash = 14695981039346656037ull;
    hash = mixSpan(hash, std::span<const GpuPlacedPart>{table.parts});
    hash = mixSpan(hash, std::span<const GpuPlacedKind>{table.kinds});
    return mixSpan(hash, std::span<const GpuPlacedGroup>{table.groups});
}

// One multiply and a truncation, rounded alike on both sides (a single product cannot be fused).
std::uint32_t placedKindSlot(std::uint32_t kind, std::uint32_t variants, std::uint32_t variantSlot, float pick) {
    if (variants <= 1u) {
        return kind;
    }

    const std::uint32_t variant = std::min(static_cast<std::uint32_t>(pick * static_cast<float>(variants)), variants - 1u);
    return variant == 0u ? kind : variantSlot + variant - 1u;
}

// Transform::getMatrix's steps from the plant's turn (placedTurnedModel).
Mat4 placedPlantModel(const GpuPlacedPlant& plant) {
    return placedTurnedModel(plant, placedTurn(plant.turnRadians));
}

std::array<float, 4> placedSphere(const float sphere[4], const GpuPlacedPlant& plant) {
    return placedSphereBy(sphere, placedPlantModel(plant), plant.scale);
}

// The plant's matrix made once for both spheres.
GpuCullRecord placedCullRecord(const GpuCullRecord& part, const GpuPlacedPlant& plant) {
    GpuCullRecord record = part;
    const Mat4 model = placedPlantModel(plant);
    const std::array<float, 4> bounds = placedSphereBy(part.bounds, model, plant.scale);
    const std::array<float, 4> levelSphere = placedSphereBy(part.levelSphere, model, plant.scale);
    std::copy(bounds.begin(), bounds.end(), record.bounds);
    std::copy(levelSphere.begin(), levelSphere.end(), record.levelSphere);
    return record;
}

// Column-major in and out, as the shaders hold them. The turn made once for the matrix, the normals and the sphere.
GpuInstance placedInstance(const GpuInstance& part, const GpuPlacedPlant& plant) {
    const auto read = readColumnMajor;
    const PlacedTurn turn = placedTurn(plant.turnRadians);
    Mat4 rotation = Mat4::identity();
    rotation.values[0][0] = turn.diagonal;
    rotation.values[0][2] = turn.corner;
    rotation.values[2][0] = -turn.corner;
    rotation.values[2][2] = turn.diagonal;

    const Mat4 model = placedTurnedModel(plant, turn);
    GpuInstance instance = part;
    writeColumnMajor(model * read(part.model), instance.model);
    writeColumnMajor(rotation * read(part.normalMatrix), instance.normalMatrix);
    instance.handedness[1] = part.handedness[1] * plant.scale;
    const std::array<float, 4> levelSphere = placedSphereBy(part.levelSphere, model, plant.scale);
    std::copy(levelSphere.begin(), levelSphere.end(), instance.levelSphere);
    return instance;
}

/*
* Each group's entries gathered in item order, then plant order, then joined group by group: what the passes' prefix
* sums place (the count and scan passes' item offsets, the commands pass's group bases from the view's entries).
*/
PlacedCullResult cullPlacedOnCpu(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedItem> items,
                                 std::span<const GpuPlacedPlant> plants, std::span<const std::uint32_t> counts, const GpuPlacedView& view) {
    std::vector<std::vector<GpuPlacedEntry>> byGroup(table.groups.size());

    for (const GpuPlacedItem& item : items.subspan(view.firstItem, view.items)) {
        const GpuPlacedBlock& block = blocks[item.block];

        for (std::uint32_t thread = 0; thread < placedGroupSize; ++thread) {
            if (!blockPlant(block, thread, counts)) {
                continue;
            }

            const std::uint32_t number = block.firstPlant + thread;
            const GpuPlacedPlant& plant = plants[number];
            const GpuPlacedKind& own = table.kinds.at(block.firstKind + plant.kind);
            const GpuPlacedKind& kind = table.kinds.at(block.firstKind + placedKindSlot(plant.kind, own.variants, own.variantSlot, plant.pick));

            for (std::uint32_t each = 0; each < kind.parts; ++each) {
                const PartDraws draws = placedPartEntries(table.parts[kind.part[each]], plant, number, view.camera);

                for (int drawn = 0; drawn < 2; ++drawn) {
                    if (draws.groups[drawn] < placedMostGroups) {
                        byGroup[draws.groups[drawn]].push_back(GpuPlacedEntry{draws.entries[drawn], kind.part[each]});
                    }
                }
            }
        }
    }

    PlacedCullResult result;

    for (std::size_t group = 0; group < table.groups.size(); ++group) {
        const GpuPlacedGroup& mesh = table.groups[group];
        result.commands.push_back(GpuDrawCommand{mesh.indexCount, static_cast<std::uint32_t>(byGroup[group].size()), mesh.firstIndex, mesh.vertexOffset,
                                                 placedInstanceBase + view.entryBase + static_cast<std::uint32_t>(result.entries.size())});
        result.entries.insert(result.entries.end(), byGroup[group].begin(), byGroup[group].end());
    }

    return result;
}

/*
* Group by group: the GPU's entries read from its own command's range, both sides' sorted to count the ones in both;
* a group whose entries are the same but not in the same order is reordered.
*/
PlacedComparison comparePlacedDraws(const PlacedTable& table, std::span<const GpuPlacedPlant> plants, const GpuPlacedView& view, const PlacedCullResult& cpu,
                                    std::span<const GpuDrawCommand> gpuCommands, std::span<const GpuPlacedEntry> gpuEntries) {
    PlacedComparison comparison;
    const auto byKey = [](const GpuPlacedEntry& x, const GpuPlacedEntry& y) {
        return std::pair{x.plant, x.part} < std::pair{y.plant, y.part};
    };
    const auto near = [&](const GpuPlacedEntry& entry) {
        const std::uint32_t number = entry.plant & DRAW_LIST_INSTANCE_MASK;

        if (number >= plants.size() || entry.part >= table.parts.size()) {
            return false;
        }

        const GpuPlacedPart& part = table.parts[entry.part];
        return nearACameraEdge(placedCullRecord(part.record, plants[number]), part.levels, view.camera);
    };
    // A command's entries: the GPU's count from every view's first, the CPU's from the view's own.
    const auto range = [](std::span<const GpuPlacedEntry> entries, const GpuDrawCommand& command, std::uint32_t base) {
        const std::size_t first = std::min<std::size_t>(command.firstInstance - std::min(command.firstInstance, base), entries.size());
        return entries.subspan(first, std::min<std::size_t>(command.instanceCount, entries.size() - first));
    };

    for (std::size_t group = 0; group < cpu.commands.size(); ++group) {
        const GpuDrawCommand& mine = cpu.commands[group];
        const GpuDrawCommand theirs = group < gpuCommands.size() ? gpuCommands[group] : GpuDrawCommand{};

        if (std::tie(mine.indexCount, mine.instanceCount, mine.firstIndex, mine.vertexOffset, mine.firstInstance) !=
            std::tie(theirs.indexCount, theirs.instanceCount, theirs.firstIndex, theirs.vertexOffset, theirs.firstInstance)) {
            ++comparison.commandsDiffering;
        }

        const std::span<const GpuPlacedEntry> cpuEntries = range(cpu.entries, mine, placedInstanceBase + view.entryBase);
        const std::span<const GpuPlacedEntry> gpuGroup = range(gpuEntries, theirs, placedInstanceBase);
        comparison.entries += cpuEntries.size();
        std::vector<GpuPlacedEntry> a(cpuEntries.begin(), cpuEntries.end());
        std::vector<GpuPlacedEntry> b(gpuGroup.begin(), gpuGroup.end());
        const bool sameOrder = a == b;
        std::sort(a.begin(), a.end(), byKey);
        std::sort(b.begin(), b.end(), byKey);
        std::vector<GpuPlacedEntry> onlyCpu;
        std::vector<GpuPlacedEntry> onlyGpu;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(onlyCpu), byKey);
        std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(onlyGpu), byKey);
        comparison.missing += onlyCpu.size();
        comparison.extra += onlyGpu.size();
        comparison.matching += a.size() - onlyCpu.size();
        comparison.nearAnEdge += static_cast<std::size_t>(std::count_if(onlyCpu.begin(), onlyCpu.end(), near) + std::count_if(onlyGpu.begin(), onlyGpu.end(), near));

        if (!sameOrder && onlyCpu.empty() && onlyGpu.empty()) {
            ++comparison.reordered;
        }
    }

    return comparison;
}

// Agreeing: nothing missing or extra but near an edge, nothing reordered, and no command differing but by those.
std::string describePlacedComparison(const PlacedComparison& comparison) {
    const std::size_t differing = comparison.missing + comparison.extra;
    const bool agree = differing == comparison.nearAnEdge && comparison.reordered == 0 && (comparison.commandsDiffering == 0 || differing > 0);
    std::ostringstream text;
    text << "Placed draws: " << comparison.entries << " entries, " << comparison.matching << " matching, " << comparison.missing << " missing, "
         << comparison.extra << " extra (" << comparison.nearAnEdge << " near an edge), " << comparison.reordered << " groups reordered, "
         << comparison.commandsDiffering << " commands differing: " << (agree ? "they agree" : "they DISAGREE");
    return text.str();
}
