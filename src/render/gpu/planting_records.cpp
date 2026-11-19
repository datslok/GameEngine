#include "render/gpu/planting_records.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
    constexpr std::uint32_t headerWords = sizeof(GpuPlantingMapsHeader) / sizeof(std::uint32_t);

    std::uint32_t wordCount(const std::vector<std::uint32_t>& words) {
        return static_cast<std::uint32_t>(words.size()) - headerWords;
    }

    void appendFloat(std::vector<std::uint32_t>& words, float value) {
        words.push_back(std::bit_cast<std::uint32_t>(value));
    }

    // Values of `bits` bits packed from the low end of each word, the last word's rest zero.
    template <typename Value>
    void appendPacked(std::vector<std::uint32_t>& words, const std::vector<Value>& values, unsigned bits) {
        const std::size_t each = 32 / bits;

        for (std::size_t first = 0; first < values.size(); first += each) {
            std::uint32_t word = 0;

            for (std::size_t i = 0; i < each && first + i < values.size(); ++i) {
                word |= static_cast<std::uint32_t>(values[first + i]) << (bits * i);
            }

            words.push_back(word);
        }
    }

    template <typename Value>
    std::vector<Value> readPacked(std::span<const std::uint32_t> words, std::uint32_t first, std::size_t count, unsigned bits) {
        const std::size_t each = 32 / bits;
        const std::uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1u;
        std::vector<Value> values(count);

        for (std::size_t i = 0; i < count; ++i) {
            values[i] = static_cast<Value>((words[first + i / each] >> (bits * (i % each))) & mask);
        }

        return values;
    }

    float readFloat(std::span<const std::uint32_t> words, std::uint32_t at) {
        return std::bit_cast<float>(words[at]);
    }

    // A plant's bits for the comparison: everything the GPU writes.
    using PlantBits = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;

    PlantBits bitsOf(const GpuPlacedPlant& plant) {
        return {std::bit_cast<std::uint32_t>(plant.foot[0]), std::bit_cast<std::uint32_t>(plant.foot[1]), std::bit_cast<std::uint32_t>(plant.foot[2]),
                std::bit_cast<std::uint32_t>(plant.scale),   std::bit_cast<std::uint32_t>(plant.turnRadians), std::bit_cast<std::uint32_t>(plant.pick),
                plant.kind,                                  std::bit_cast<std::uint32_t>(plant.threshold)};
    }

    // The cell whose box holds a place along one axis: plantingCellBox's west <= x < east, exactly.
    int cellAlong(float place, float cellMetres) {
        int cell = static_cast<int>(std::floor(place / cellMetres));

        if (place < static_cast<float>(cell) * cellMetres) {
            --cell;
        } else if (place >= static_cast<float>(cell + 1) * cellMetres) {
            ++cell;
        }

        return cell;
    }
}

std::vector<std::uint32_t> packPlantingMaps(const PlantingMaps& maps) {
    if (maps.pathInverseLengths.size() + 1 < maps.pathPoints.size()) {
        throw std::invalid_argument("packPlantingMaps: a path piece without its inverse length");
    }

    GpuPlantingMapsHeader header{};
    header.columns = maps.columns;
    header.rows = maps.rows;
    header.texelMetres = maps.texelMetres;
    header.inverseTexel = maps.inverseTexel;
    header.halfWidth = maps.halfWidth;
    header.northmost = maps.northmost;
    header.texelReach = maps.texelReach;
    header.corridorClearance = maps.corridorClearance;
    std::vector<std::uint32_t> words(headerWords, 0);

    header.axisFirst = wordCount(words);
    header.axisCount = static_cast<std::uint32_t>(maps.axis.size());

    for (const float x : maps.axis) {
        appendFloat(words, x);
    }

    header.ecotopesFirst = wordCount(words);

    for (const std::array<std::uint8_t, 4>& texel : maps.ecotopes) {
        words.push_back(static_cast<std::uint32_t>(texel[0]) | (static_cast<std::uint32_t>(texel[1]) << 8) | (static_cast<std::uint32_t>(texel[2]) << 16) |
                        (static_cast<std::uint32_t>(texel[3]) << 24));
    }

    header.gapsFirst = wordCount(words);
    appendPacked(words, maps.gaps, 8);
    header.pathFirst = wordCount(words);
    appendPacked(words, maps.path, 16);
    header.clearanceFirst = wordCount(words);
    appendPacked(words, maps.clearance, 16);

    header.pathPointsFirst = wordCount(words);
    header.pathPieces = maps.pathPoints.size() > 1 ? static_cast<std::uint32_t>(maps.pathPoints.size() - 1) : 0;

    for (const Vec2& point : maps.pathPoints) {
        appendFloat(words, point.x);
        appendFloat(words, point.y);
    }

    header.pathInverseFirst = wordCount(words);

    for (std::uint32_t piece = 0; piece < header.pathPieces; ++piece) {
        appendFloat(words, maps.pathInverseLengths[piece]);
    }

    header.capsFirst = wordCount(words);
    header.capCount = static_cast<std::uint32_t>(maps.caps.size());
    const std::size_t capRecords = words.size();
    words.resize(words.size() + maps.caps.size() * PLANTING_CAP_WORDS, 0);

    for (std::size_t cap = 0; cap < maps.caps.size(); ++cap) {
        const PlantingCap& each = maps.caps[cap];
        std::uint32_t* record = &words[capRecords + cap * PLANTING_CAP_WORDS];
        record[0] = std::bit_cast<std::uint32_t>(each.reach);
        record[1] = std::bit_cast<std::uint32_t>(each.origin.x);
        record[2] = std::bit_cast<std::uint32_t>(each.origin.y);
        record[3] = static_cast<std::uint32_t>(each.columns);
        record[4] = static_cast<std::uint32_t>(each.rows);
        record[5] = wordCount(words);
        appendPacked(words, each.heights, 16);
    }

    header.flags = (maps.ecotopes.empty() ? 0u : PLANTING_HAS_ECOTOPES) | (maps.gaps.empty() ? 0u : PLANTING_HAS_GAPS) |
                   (maps.path.empty() ? 0u : PLANTING_HAS_PATH) | (maps.clearance.empty() ? 0u : PLANTING_HAS_CLEARANCE);
    std::memcpy(words.data(), &header, sizeof(header));
    return words;
}

PlantingMaps unpackPlantingMaps(std::span<const std::uint32_t> words) {
    GpuPlantingMapsHeader header{};
    std::memcpy(&header, words.data(), sizeof(header));
    const std::span<const std::uint32_t> body = words.subspan(headerWords);
    const std::size_t texels = static_cast<std::size_t>(header.columns) * static_cast<std::size_t>(header.rows);
    PlantingMaps maps;
    maps.columns = header.columns;
    maps.rows = header.rows;
    maps.texelMetres = header.texelMetres;
    maps.inverseTexel = header.inverseTexel;
    maps.halfWidth = header.halfWidth;
    maps.northmost = header.northmost;
    maps.texelReach = header.texelReach;
    maps.corridorClearance = header.corridorClearance;

    for (std::uint32_t row = 0; row < header.axisCount; ++row) {
        maps.axis.push_back(readFloat(body, header.axisFirst + row));
    }

    if ((header.flags & PLANTING_HAS_ECOTOPES) != 0) {
        for (std::size_t texel = 0; texel < texels; ++texel) {
            const std::uint32_t word = body[header.ecotopesFirst + texel];
            maps.ecotopes.push_back({static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word >> 16),
                                     static_cast<std::uint8_t>(word >> 24)});
        }
    }

    if ((header.flags & PLANTING_HAS_GAPS) != 0) {
        maps.gaps = readPacked<std::uint8_t>(body, header.gapsFirst, texels, 8);
    }

    if ((header.flags & PLANTING_HAS_PATH) != 0) {
        maps.path = readPacked<std::uint16_t>(body, header.pathFirst, texels, 16);
    }

    if ((header.flags & PLANTING_HAS_CLEARANCE) != 0) {
        maps.clearance = readPacked<std::uint16_t>(body, header.clearanceFirst, texels, 16);
    }

    const std::uint32_t points = header.pathPieces > 0 ? header.pathPieces + 1 : (header.pathInverseFirst - header.pathPointsFirst) / 2;

    for (std::uint32_t point = 0; point < points; ++point) {
        maps.pathPoints.push_back(Vec2{readFloat(body, header.pathPointsFirst + 2 * point), readFloat(body, header.pathPointsFirst + 2 * point + 1)});
    }

    for (std::uint32_t piece = 0; piece < header.pathPieces; ++piece) {
        maps.pathInverseLengths.push_back(readFloat(body, header.pathInverseFirst + piece));
    }

    for (std::uint32_t cap = 0; cap < header.capCount; ++cap) {
        const std::uint32_t at = header.capsFirst + cap * PLANTING_CAP_WORDS;
        PlantingCap each;
        each.reach = readFloat(body, at);
        each.origin = Vec2{readFloat(body, at + 1), readFloat(body, at + 2)};
        each.columns = static_cast<int>(body[at + 3]);
        each.rows = static_cast<int>(body[at + 4]);
        each.heights = readPacked<std::uint16_t>(body, body[at + 5], static_cast<std::size_t>(each.columns) * static_cast<std::size_t>(each.rows), 16);
        maps.caps.push_back(std::move(each));
    }

    return maps;
}

std::vector<GpuPlantingLayer> packPlantingLayers(const std::vector<PlantingLayerRecord>& layers) {
    checkPlantingLayers(layers);
    std::vector<GpuPlantingLayer> packed;

    for (const PlantingLayerRecord& layer : layers) {
        if (layer.keepAway.size() > static_cast<std::size_t>(plantingMostKeepAways)) {
            throw std::invalid_argument("packPlantingLayers: a layer keeps away from more than PLANTING_MOST_KEEP_AWAYS layers");
        }

        GpuPlantingLayer gpu{};
        std::copy(layer.sharePerWeight.begin(), layer.sharePerWeight.end(), gpu.sharePerWeight);
        gpu.gapStep = layer.gapStep;
        gpu.nearPath = layer.nearPath;
        gpu.farPath = layer.farPath;
        gpu.farShare = layer.farShare;
        gpu.inversePathSpan = layer.inversePathSpan;
        gpu.footprint = layer.footprint;
        gpu.jitterMetres = layer.jitterMetres;
        gpu.pathClearance = layer.pathClearance;
        gpu.under = layer.under;
        gpu.seed[0] = static_cast<std::uint32_t>(layer.seed);
        gpu.seed[1] = static_cast<std::uint32_t>(layer.seed >> 32);
        gpu.kinds = layer.kinds;
        gpu.keepAwayCount = static_cast<std::uint32_t>(layer.keepAway.size());
        std::fill(std::begin(gpu.kindShares), std::end(gpu.kindShares), 2.0f);
        std::copy(layer.kindShares.begin(), layer.kindShares.end(), gpu.kindShares);
        gpu.leastScale = layer.leastScale;
        gpu.scaleRange = layer.scaleRange;
        gpu.footRingPerScale = layer.footRingPerScale;
        gpu.footSink = layer.footSink;
        gpu.spacingShare = layer.spacingShare;
        gpu.spacingRings = plantingSpacingRings(layer);
        gpu.sizes = static_cast<std::uint32_t>(layer.reachPerScale.size());
        gpu.sharedKinds = layer.kindShares.empty() ? 0u : 1u;

        for (std::size_t keep = 0; keep < layer.keepAway.size(); ++keep) {
            gpu.keepLayer[keep] = layer.keepAway[keep].layer;
            gpu.keepRadius[keep] = layer.keepAway[keep].radius;
            gpu.keepReach[keep] = plantingKeepAwayReach(layer.keepAway[keep], layers);
            gpu.keepCrown[keep] = layer.keepAway[keep].crown ? 1u : 0u;
        }

        std::copy(layer.reachPerScale.begin(), layer.reachPerScale.end(), gpu.reachPerScale);
        std::copy(layer.centrePerScale.begin(), layer.centrePerScale.end(), gpu.centrePerScale);
        packed.push_back(gpu);
    }

    return packed;
}

std::vector<GpuPlantingJob> packPlantingJobs(const std::vector<PlantingLayerRecord>& layers, const PlantingSlotLayout& layout,
                                             std::span<const PlantingCellMade> made, float cellMetres) {
    std::vector<GpuPlantingJob> jobs;
    jobs.reserve(made.size() * layers.size());
    const auto lattice = [](std::int32_t (&into)[4], const PlantingLattice& from) {
        into[0] = from.firstColumn;
        into[1] = from.firstRow;
        into[2] = from.columns;
        into[3] = from.rows;
    };

    for (std::size_t cell = 0; cell < made.size(); ++cell) {
        const PlantingBox box = plantingCellBox(made[cell].cell, cellMetres);
        const std::vector<PlantingLayerBoxes> boxes = plantingLayerBoxes(layers, box);
        const auto slot = static_cast<std::uint32_t>(made[cell].slot);

        for (std::size_t which = 0; which < layers.size(); ++which) {
            const float footprint = layers[which].footprint;
            const PlantingLattice candidates = plantingLatticeOver(boxes[which].candidates, footprint);
            const PlantingLattice inBox = plantingLatticeOver(box, footprint);

            if (candidates.count() > layout.scratchCapacity[which] || inBox.count() > layout.layerCapacity[which]) {
                throw std::logic_error("packPlantingJobs: a cell's lattice outgrew its room");
            }

            GpuPlantingJob job{};
            lattice(job.candidates, candidates);
            lattice(job.standing, plantingLatticeOver(boxes[which].standing, footprint));
            lattice(job.box, inBox);
            job.bounds[0] = box.west;
            job.bounds[1] = box.east;
            job.bounds[2] = box.north;
            job.bounds[3] = box.south;
            job.candidateFirst = static_cast<std::uint32_t>(cell) * layout.candidatesACell + layout.scratchFirst[which];
            job.plantFirst = slot * layout.plantsASlot + layout.layerFirst[which];
            job.countPlace = slot * static_cast<std::uint32_t>(layers.size()) + static_cast<std::uint32_t>(which);
            job.capacity = layout.layerCapacity[which];
            jobs.push_back(job);
        }
    }

    return jobs;
}

GpuPlacedPlant packPlacedPlant(const PlantedPlant& plant) {
    return GpuPlacedPlant{{plant.foot.x, plant.foot.y, plant.foot.z}, plant.scale, plant.turnRadians, plant.pick, static_cast<std::uint32_t>(plant.kind),
                          plant.threshold};
}

/*
* In order where both have the same plants in the same order; otherwise each plant matched by its bits (a multiset), the
* rest missing or extra, and a cell with nothing missing or extra but another order counted reordered.
*/
void comparePlantingCell(PlantingComparison& comparison, const std::vector<std::vector<PlantedPlant>>& cpu,
                         const std::vector<std::vector<GpuPlacedPlant>>& gpu) {
    ++comparison.cells;
    const std::size_t layers = std::max(cpu.size(), gpu.size());
    comparison.layers.resize(std::max(comparison.layers.size(), layers));

    for (std::size_t which = 0; which < layers; ++which) {
        std::vector<PlantBits> expected;
        std::vector<PlantBits> found;

        if (which < cpu.size()) {
            for (const PlantedPlant& plant : cpu[which]) {
                expected.push_back(bitsOf(packPlacedPlant(plant)));
            }
        }

        if (which < gpu.size()) {
            for (const GpuPlacedPlant& plant : gpu[which]) {
                found.push_back(bitsOf(plant));
            }
        }

        PlantingLayerComparison& layer = comparison.layers[which];

        if (expected == found) {
            layer.matching += expected.size();
            continue;
        }

        std::sort(expected.begin(), expected.end());
        std::sort(found.begin(), found.end());
        std::vector<PlantBits> both;
        std::set_intersection(expected.begin(), expected.end(), found.begin(), found.end(), std::back_inserter(both));
        layer.matching += both.size();
        layer.missing += expected.size() - both.size();
        layer.extra += found.size() - both.size();
        layer.reordered += both.size() == expected.size() && both.size() == found.size() ? 1 : 0;
    }
}

std::map<PlantingCell, std::vector<std::vector<PlantedPlant>>> plantingByCell(const std::vector<std::vector<PlantedPlant>>& planted,
                                                                               float cellMetres) {
    std::map<PlantingCell, std::vector<std::vector<PlantedPlant>>> cells;

    for (std::size_t which = 0; which < planted.size(); ++which) {
        for (const PlantedPlant& plant : planted[which]) {
            std::vector<std::vector<PlantedPlant>>& layers = cells[PlantingCell{cellAlong(plant.foot.x, cellMetres), cellAlong(plant.foot.z, cellMetres)}];
            layers.resize(planted.size());
            layers[which].push_back(plant);
        }
    }

    return cells;
}

PlantingBox plantingCellsBox(std::span<const PlantingCell> cells, float cellMetres) {
    if (cells.empty()) {
        return PlantingBox{};
    }

    PlantingBox box = plantingCellBox(cells.front(), cellMetres);

    for (const PlantingCell& cell : cells) {
        const PlantingBox each = plantingCellBox(cell, cellMetres);
        box = PlantingBox{std::min(box.west, each.west), std::max(box.east, each.east), std::min(box.north, each.north), std::max(box.south, each.south)};
    }

    return box;
}

std::string describePlantingComparison(const PlantingComparison& comparison) {
    static constexpr const char* problemNames[PLANTING_PROBLEMS] = {"read past an earlier layer", "wider than every cap", "past a slot's room"};
    std::ostringstream text;
    bool agree = true;
    text << "Placement: " << comparison.cells << " cells.";

    for (std::size_t which = 0; which < comparison.layers.size(); ++which) {
        const PlantingLayerComparison& layer = comparison.layers[which];
        text << " Layer " << which << ": " << layer.matching << " matching";

        if (layer.missing > 0 || layer.extra > 0 || layer.reordered > 0) {
            text << ", " << layer.missing << " missing, " << layer.extra << " extra, " << layer.reordered << " cells reordered";
            agree = false;
        }

        text << ".";
    }

    for (std::size_t problem = 0; problem < comparison.problems.size(); ++problem) {
        if (comparison.problems[problem] > 0) {
            text << " " << comparison.problems[problem] << " plants " << problemNames[problem] << ".";
            agree = false;
        }
    }

    text << (agree ? " The GPU and the CPU agree." : " They DIFFER.");
    return text.str();
}
