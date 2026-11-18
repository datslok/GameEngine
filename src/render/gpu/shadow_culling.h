#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"
#include "scene/level_of_detail.h"

#include <cstdint>
#include <vector>

/*
* Shadow tiles as views of the GPU culling (docs/research/GPUDRIVEN.md, stage 5; docs/architecture/gpu-driven.md). The
* GPU finds a redrawn tile's casters; the CPU decides which tiles to redraw from what the scene announces changed, not
* from a fingerprint of every caster, so its work follows the changes rather than everything there is:
*
* - the tile's view or the level settings changed (the key's tile and its levels);
* - the still draws were built afresh, or one built in place touches the tile where it was or where it is;
* - a moving draw it takes changed (the movers' fingerprint in its key; those that moved kept apart where it keeps them so);
* - its glass changed (the glass's fingerprint, from the CPU's short glass list);
* - it sways (a new key every frame).
*/

// A tile's casters as the CPU finds them, the GPU's reference: of inReach (castersInReach for its reach), those in its
// view, at least its smallest caster's size and within its cascade.
std::vector<std::size_t> shadowTileCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& inReach, const ShadowTile& tile);

// A tile as a view: its frustum, levels by its own texels (tileLevelView), reach, smallest caster and cascade; the moved
// casters in its second list where it keeps its movers apart.
GpuCameraCull packShadowCull(const ShadowTile& tile, const LevelOfDetailSettings& settings);

// What a tile needs this frame: whether its still casters are drawn again, the key to remember once they are, and the
// moving draws it takes (standing in still, moved in moving where it keeps them apart; all in still otherwise), and the
// tile as a view (packShadowCull).
struct ShadowTileNeeds {
    GpuCameraCull view;
    bool redraw = false;
    std::uint64_t key = 0;
    ShadowCasterSplit movers;
};

/*
* The tile's needs from the frame's table (CameraCullFrame: its still changes, the moving draws' records after the
* still ones in draws), its glass (draws tinting it) and whether it sways this frame (frame then makes its key new).
*/
ShadowTileNeeds shadowTileNeeds(const ShadowTile& tile, const LevelOfDetailSettings& settings, const CameraCullFrame& table, const std::vector<DrawItem>& draws,
                                const std::vector<std::size_t>& glass, bool sways, std::uint64_t frame, const ShadowTileMemory& memory);
