#include "render/gpu/still_instances.h"
#include "render/gpu/column_major.h"

#include <algorithm>

namespace {
    constexpr std::size_t drawsPerPiece = 256;

    // Old entries a table may gather beyond twice its whole packing's before it is packed afresh.
    constexpr std::size_t spareMaterialEntries = 64;
}

std::vector<std::array<float, 16>> packPreviousModels(std::span<const DrawItem> draws, JobSystem& jobs) {
    std::vector<std::array<float, 16>> previous(draws.size());
    jobs.parallelFor(draws.size(), drawsPerPiece, [&](std::size_t draw) {
        writeColumnMajor(draws[draw].previousModel.value_or(draws[draw].model), previous[draw].data());
    });
    return previous;
}

namespace {
    // How many of the slots from first on (to end) the ranges cover, joined.
    std::size_t coveredFrom(const std::vector<DrawRange>& ranges, std::size_t first, std::size_t end) {
        std::size_t covered = 0;

        for (const DrawRange& range : joinDrawRanges(ranges)) {
            const std::size_t from = std::max(range.first, first);
            const std::size_t to = std::min(range.first + range.count, end);
            covered += to > from ? to - from : 0;
        }

        return covered;
    }
}

/*
* Whole when the frame does not follow the last one packed, when it has fewer still draws, when its built ranges miss
* slots added at the end (scene/still_draws.h: a cell's new block), or when it built every still draw; otherwise its
* built ranges (the new slots among them), into the kept table, unless that has gathered too many old entries.
*/
std::vector<DrawRange> StillInstances::update(const FrameDescription& frame, JobSystem& jobs) {
    if (frame.stillDraws == 0) {
        instances.clear();
        previousModels.clear();
        materials = MaterialTable{};
        firstNewMaterial = 0;
        packedVersion = frame.stillVersion;
        return {};
    }

    firstNewMaterial = materials.getEntries().size();

    if (packedVersion == frame.stillVersion && instances.size() == frame.stillDraws) {
        return {};
    }

    const std::vector<DrawRange> built = joinDrawRanges(frame.builtStillDraws);
    const std::size_t builtCount = coveredFrom(built, 0, frame.stillDraws);
    const std::size_t had = instances.size();
    const bool follows = packedVersion != 0 && packedVersion == frame.stillPreviousVersion && had <= frame.stillDraws &&
                         coveredFrom(built, had, frame.stillDraws) == frame.stillDraws - had;
    std::vector<DrawRange> packed;

    if (!follows || builtCount >= frame.stillDraws) {
        packAll(frame, jobs);
        packed.push_back(DrawRange{0, frame.stillDraws});
    } else {
        instances.resize(frame.stillDraws);
        previousModels.resize(frame.stillDraws);

        for (const DrawRange& range : built) {
            packRange(frame, range, jobs);
        }

        if (materials.getEntries().size() > 2 * entriesWhenWhole + spareMaterialEntries) {
            packAll(frame, jobs);
            packed.push_back(DrawRange{0, frame.stillDraws});
        } else {
            packed = built;
        }
    }

    packedVersion = frame.stillVersion;
    return packed;
}

std::size_t StillInstances::getFirstNewMaterial() const {
    return firstNewMaterial;
}

void StillInstances::packAll(const FrameDescription& frame, JobSystem& jobs) {
    const std::span<const DrawItem> still = std::span<const DrawItem>{frame.draws}.first(frame.stillDraws);
    materials = MaterialTable{};
    instances = packInstances(still, materials, jobs);
    previousModels = packPreviousModels(still, jobs);
    entriesWhenWhole = materials.getEntries().size();
    firstNewMaterial = 0;
}

void StillInstances::packRange(const FrameDescription& frame, const DrawRange& range, JobSystem& jobs) {
    const std::span<const DrawItem> draws = std::span<const DrawItem>{frame.draws}.subspan(range.first, range.count);
    const std::vector<GpuInstance> packed = packInstances(draws, materials, jobs);
    const std::vector<std::array<float, 16>> previous = packPreviousModels(draws, jobs);
    std::copy(packed.begin(), packed.end(), instances.begin() + static_cast<std::ptrdiff_t>(range.first));
    std::copy(previous.begin(), previous.end(), previousModels.begin() + static_cast<std::ptrdiff_t>(range.first));
}

std::span<const GpuInstance> StillInstances::getInstances() const {
    return instances;
}

std::span<const std::array<float, 16>> StillInstances::getPreviousModels() const {
    return previousModels;
}

const MaterialTable& StillInstances::getMaterials() const {
    return materials;
}

SceneRecords packMovingDraws(const FrameDescription& frame, const StillInstances& still, JobSystem& jobs) {
    SceneRecords records;
    records.materials = MaterialTable::over(still.getMaterials());
    const std::span<const DrawItem> moving = std::span<const DrawItem>{frame.draws}.subspan(frame.stillDraws);
    records.moving = packInstances(moving, records.materials, jobs);
    records.movingPrevious = packPreviousModels(moving, jobs);
    records.planted = packInstances(frame.plantedParts, records.materials, jobs);
    return records;
}
