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

/*
* Whole when the frame does not follow the last one packed, when its count differs, or when it built every still draw;
* otherwise its built ranges, into the kept table, unless that has gathered too many old entries.
*/
std::vector<DrawRange> StillInstances::update(const FrameDescription& frame, JobSystem& jobs) {
    if (frame.stillDraws == 0) {
        instances.clear();
        previousModels.clear();
        materials = MaterialTable{};
        packedVersion = frame.stillVersion;
        return {};
    }

    if (packedVersion == frame.stillVersion && instances.size() == frame.stillDraws) {
        return {};
    }

    std::size_t built = 0;

    for (const DrawRange& range : frame.builtStillDraws) {
        built += range.count;
    }

    const bool follows = packedVersion != 0 && packedVersion == frame.stillPreviousVersion && instances.size() == frame.stillDraws;
    std::vector<DrawRange> packed;

    if (!follows || built >= frame.stillDraws) {
        packAll(frame, jobs);
        packed.push_back(DrawRange{0, frame.stillDraws});
    } else {
        for (const DrawRange& range : frame.builtStillDraws) {
            packRange(frame, range, jobs);
        }

        if (materials.getEntries().size() > 2 * entriesWhenWhole + spareMaterialEntries) {
            packAll(frame, jobs);
            packed.push_back(DrawRange{0, frame.stillDraws});
        } else {
            packed = frame.builtStillDraws;
        }
    }

    packedVersion = frame.stillVersion;
    return packed;
}

void StillInstances::packAll(const FrameDescription& frame, JobSystem& jobs) {
    const std::span<const DrawItem> still = std::span<const DrawItem>{frame.draws}.first(frame.stillDraws);
    materials = MaterialTable{};
    instances = packInstances(still, materials, jobs);
    previousModels = packPreviousModels(still, jobs);
    entriesWhenWhole = materials.getEntries().size();
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
    records.materials = still.getMaterials();
    const std::span<const DrawItem> moving = std::span<const DrawItem>{frame.draws}.subspan(frame.stillDraws);
    records.moving = packInstances(moving, records.materials, jobs);
    records.movingPrevious = packPreviousModels(moving, jobs);
    return records;
}
