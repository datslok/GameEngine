#include "render/gpu/shadow_culling.h"
#include "render/gpu/wind.h"
#include "scene/culling.h"

#include <algorithm>
#include <cstring>

namespace {
    // FNV-1a over a value's bytes, carried on from hash (shadow_cache's fingerprints use the same).
    template<typename Value>
    std::uint64_t mixBytes(std::uint64_t hash, const Value& value) {
        unsigned char bytes[sizeof(Value)];
        std::memcpy(bytes, &value, sizeof(Value));

        for (unsigned char byte : bytes) {
            hash = (hash ^ byte) * 1099511628211ull;
        }

        return hash;
    }
}

/*
* The thinning last (shadowThinKeeps), from each draw's record against the tile's view, so both sides use one function;
* only the view's frustum, cascade and band matter to it.
*/
std::vector<std::size_t> shadowTileCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& inReach, const ShadowTile& tile) {
    const Frustum view = Frustum::fromClipMatrix(tile.matrix, ClipDepth::OneToZero);
    std::vector<std::size_t> casters = castersForCascade(draws, castersAtLeast(draws, shadowCasters(draws, inReach, view), tile.smallestCaster), tile.sunCascade);
    const GpuCameraCull thinning = packShadowCull(tile, LevelOfDetailSettings{});
    std::erase_if(casters, [&](std::size_t index) { return !shadowThinKeeps(packCullRecord(draws[index]), thinning); });
    return casters;
}

GpuCameraCull packShadowCull(const ShadowTile& tile, const LevelOfDetailSettings& settings) {
    GpuCameraCull view = packCameraCull(Frustum::fromClipMatrix(tile.matrix, ClipDepth::OneToZero), tileLevelView(tile, settings), false, false, 0.0f);
    view.flags = CAMERA_CULL_SHADOW | (tile.separateMovers ? CAMERA_CULL_MOTION : 0u);
    view.reach[0] = tile.reach.centre.x;
    view.reach[1] = tile.reach.centre.y;
    view.reach[2] = tile.reach.centre.z;
    view.reach[3] = tile.reach.radius;
    view.smallestCaster = tile.smallestCaster;
    view.cascade = tile.sunCascade;
    view.thinning[0] = tile.thinStart;
    view.thinning[1] = tile.thinEnd;
    return view;
}

GpuCameraCull packVirtualShadowCull(const Mat4& box, const LevelView& cameraLevels) {
    GpuCameraCull view = packCameraCull(Frustum::fromClipMatrix(box, ClipDepth::OneToZero), cameraLevels, false, false, 0.0f);
    view.flags = CAMERA_CULL_SHADOW;
    return view;
}

/*
* The movers it takes are found from their records (shadowTakes, the GPU's own test), the changed still draws likewise
* where they were and where they are; the key is the movers' and glass's fingerprint with the tile's levels mixed in.
*/
ShadowTileNeeds shadowTileNeeds(const ShadowTile& tile, const LevelOfDetailSettings& settings, const CameraCullFrame& table, const std::vector<DrawItem>& draws,
                                const std::vector<std::size_t>& glass, bool sways, std::uint64_t frame, const ShadowTileMemory& memory,
                                const PlantedShadowChanges& planted) {
    ShadowTileNeeds needs;
    needs.view = packShadowCull(tile, settings);
    const GpuCameraCull& view = needs.view;
    const std::size_t firstMoving = table.stillRecords.size();
    std::vector<std::size_t> movers;

    for (std::size_t mover = 0; mover < table.movingRecords.size(); ++mover) {
        if (shadowTakes(table.movingRecords[mover], view)) {
            movers.push_back(firstMoving + mover);
        }
    }

    needs.movers = tile.separateMovers ? splitMovingCasters(draws, movers) : ShadowCasterSplit{movers, {}};

    const LevelView levels = tileLevelView(tile, settings);
    const std::uint64_t fingerprint = tintedShadowSignature(draws, needs.movers.still, glass, &levels);
    needs.key = swayingShadowSignature(mixBytes(mixBytes(fingerprint, view.levels), view.thinning), sways, frame);

    const auto takes = [&view](const GpuCullRecord& record) {
        return shadowTakes(record, view);
    };
    const bool touched = std::any_of(table.stillChanges.begin(), table.stillChanges.end(), takes) ||
                         std::any_of(planted.cells.begin(), planted.cells.end(), takes);
    needs.redraw = table.stillRebuilt || planted.all || touched || !memory.isCurrent(tile, needs.key);
    return needs;
}

bool keepsMoversApart(const ShadowTile& tile, const CameraCullFrame& table, bool sways, bool hasStillCopy) {
    if (tile.separateMovers) {
        return true;
    }

    if (tile.sunCascade < 0 || sways) {
        return false;
    }

    if (hasStillCopy) {
        return true;
    }

    const GpuCameraCull view = packShadowCull(tile, LevelOfDetailSettings{});
    return std::any_of(table.movingRecords.begin(), table.movingRecords.end(), [&view](const GpuCullRecord& record) { return shadowTakes(record, view); });
}
