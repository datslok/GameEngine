#include "render/gpu/placed_drawing.h"
#include "render/gpu/column_major.h"
#include "render/gpu/transparency.h"
#include "scene/material.h"

#include <algorithm>
#include <cmath>
#include <iterator>
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

    // Whether a block's thread has a plant to cull (placedBlockPlant): within its slot's count and in the region.
    bool blockPlant(const GpuPlacedBlock& block, std::uint32_t thread, std::span<const GpuPlacedPlant> plants, std::span<const std::uint32_t> counts,
                    const PlantingMaps& maps, const GpuPlacedView& view) {
        if (block.firstInLayer + thread >= counts[block.countPlace]) {
            return false;
        }

        const GpuPlacedPlant& plant = plants[block.firstPlant + thread];
        const PlantedRegion region{view.region[0], view.region[1], view.region[2]};
        return insidePlantedRegion(maps, region, Vec3{plant.foot[0], plant.foot[1], plant.foot[2]});
    }
}

/*
* Parts sorted by style (stable, so a kind's parts keep their order within a style), each given its slots' groups in
* turn, so the groups too run style by style; the kinds name their parts' new places. Each style's run is one
* multi-draw (the drawing order is the groups' own).
*/
PlacedTable buildPlacedTable(std::span<const DrawItem> parts, std::span<const PlantedKindParts> kinds, std::size_t layerCount,
                             const std::function<MeshPlacement(MeshHandle)>& placement) {
    PlacedTable table;
    table.kinds.resize(layerCount * static_cast<std::size_t>(plantingMostKinds));
    table.drawnLayers.assign(layerCount, false);
    std::vector<std::size_t> order;

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
        }

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

                group = GpuPlacedGroup{where.indexCount, where.firstIndex, static_cast<std::int32_t>(where.firstVertex), 0};
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

    // Each kind names its parts' places, in its own order (the parts run by style, so a kind's are not one run).
    for (const PlantedKindParts& kind : kinds) {
        GpuPlacedKind& placed = table.kinds[static_cast<std::size_t>(kind.layer * plantingMostKinds + kind.kind)];
        placed.parts = static_cast<std::uint32_t>(kind.count);

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

std::vector<GpuPlacedBlock> placedBlocks(const std::map<PlantingCell, int>& held, const PlantingSlotLayout& layout, const PlacedTable& table) {
    std::vector<GpuPlacedBlock> blocks;
    const auto layers = static_cast<std::uint32_t>(layout.layerFirst.size());

    for (const auto& [cell, slot] : held) {
        for (std::uint32_t layer = 0; layer < layers && layer < table.drawnLayers.size(); ++layer) {
            if (!table.drawnLayers[layer]) {
                continue;
            }

            for (std::uint32_t first = 0; first < layout.layerCapacity[layer]; first += placedGroupSize) {
                blocks.push_back(GpuPlacedBlock{static_cast<std::uint32_t>(slot) * layout.plantsASlot + layout.layerFirst[layer] + first,
                                                static_cast<std::uint32_t>(slot) * layers + layer, first,
                                                layer * static_cast<std::uint32_t>(plantingMostKinds)});
            }
        }
    }

    return blocks;
}

std::uint32_t placedEntryRoom(std::size_t blocks, const PlacedTable& table) {
    return static_cast<std::uint32_t>(blocks * placedGroupSize * 2 * table.mostParts);
}

GpuPlacedView packPlacedView(const GpuCameraCull& camera, const PlantedRegion& region, std::size_t blocks, const PlacedTable& table) {
    GpuPlacedView view;
    view.camera = camera;
    view.region[0] = region.northmost;
    view.region[1] = region.southmost;
    view.region[2] = region.spread;
    view.blocks = static_cast<std::uint32_t>(blocks);
    view.groups = static_cast<std::uint32_t>(table.groups.size());
    view.entryRoom = placedEntryRoom(blocks, table);
    return view;
}

// Transform::getMatrix's steps with the quaternion's zero parts left out where they change nothing.
Mat4 placedPlantModel(const GpuPlacedPlant& plant) {
    const PlacedTurn turn = placedTurn(plant.turnRadians);
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

std::array<float, 4> placedSphere(const float sphere[4], const GpuPlacedPlant& plant) {
    const Mat4 model = placedPlantModel(plant);
    const float x = model.values[0][0] * sphere[0] + model.values[0][2] * sphere[2] + model.values[0][3];
    const float y = model.values[1][1] * sphere[1] + model.values[1][3];
    const float z = model.values[2][0] * sphere[0] + model.values[2][2] * sphere[2] + model.values[2][3];
    return {x, y, z, sphere[3] < 0.0f ? sphere[3] : sphere[3] * plant.scale};
}

GpuCullRecord placedCullRecord(const GpuCullRecord& part, const GpuPlacedPlant& plant) {
    GpuCullRecord record = part;
    const std::array<float, 4> bounds = placedSphere(part.bounds, plant);
    const std::array<float, 4> levelSphere = placedSphere(part.levelSphere, plant);
    std::copy(bounds.begin(), bounds.end(), record.bounds);
    std::copy(levelSphere.begin(), levelSphere.end(), record.levelSphere);
    return record;
}

// Column-major in and out, as the shaders hold them.
GpuInstance placedInstance(const GpuInstance& part, const GpuPlacedPlant& plant) {
    const auto read = [](const float* columns) {
        Mat4 matrix;

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.values[row][column] = columns[column * 4 + row];
            }
        }

        return matrix;
    };
    const PlacedTurn turn = placedTurn(plant.turnRadians);
    Mat4 rotation = Mat4::identity();
    rotation.values[0][0] = turn.diagonal;
    rotation.values[0][2] = turn.corner;
    rotation.values[2][0] = -turn.corner;
    rotation.values[2][2] = turn.diagonal;

    GpuInstance instance = part;
    writeColumnMajor(placedPlantModel(plant) * read(part.model), instance.model);
    writeColumnMajor(rotation * read(part.normalMatrix), instance.normalMatrix);
    instance.handedness[1] = part.handedness[1] * plant.scale;
    const std::array<float, 4> levelSphere = placedSphere(part.levelSphere, plant);
    std::copy(levelSphere.begin(), levelSphere.end(), instance.levelSphere);
    return instance;
}

/*
* Each group's entries gathered in block order, then plant order, then joined group by group: what the passes' prefix
* sums place (the count and scan passes' block offsets, the commands pass's group bases).
*/
PlacedCullResult cullPlacedOnCpu(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedPlant> plants,
                                 std::span<const std::uint32_t> counts, const PlantingMaps& maps, const GpuPlacedView& view) {
    std::vector<std::vector<GpuPlacedEntry>> byGroup(table.groups.size());

    for (const GpuPlacedBlock& block : blocks) {
        for (std::uint32_t thread = 0; thread < placedGroupSize; ++thread) {
            if (!blockPlant(block, thread, plants, counts, maps, view)) {
                continue;
            }

            const std::uint32_t number = block.firstPlant + thread;
            const GpuPlacedPlant& plant = plants[number];
            const GpuPlacedKind& kind = table.kinds.at(block.firstKind + plant.kind);

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
                                                 placedInstanceBase + static_cast<std::uint32_t>(result.entries.size())});
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
    const auto range = [](std::span<const GpuPlacedEntry> entries, const GpuDrawCommand& command) {
        const std::size_t first = std::min<std::size_t>(command.firstInstance - std::min(command.firstInstance, placedInstanceBase), entries.size());
        return entries.subspan(first, std::min<std::size_t>(command.instanceCount, entries.size() - first));
    };

    for (std::size_t group = 0; group < cpu.commands.size(); ++group) {
        const GpuDrawCommand& mine = cpu.commands[group];
        const GpuDrawCommand theirs = group < gpuCommands.size() ? gpuCommands[group] : GpuDrawCommand{};

        if (std::tie(mine.indexCount, mine.instanceCount, mine.firstIndex, mine.vertexOffset, mine.firstInstance) !=
            std::tie(theirs.indexCount, theirs.instanceCount, theirs.firstIndex, theirs.vertexOffset, theirs.firstInstance)) {
            ++comparison.commandsDiffering;
        }

        const std::span<const GpuPlacedEntry> cpuEntries = range(cpu.entries, mine);
        const std::span<const GpuPlacedEntry> gpuGroup = range(gpuEntries, theirs);
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
