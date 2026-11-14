#pragma once

#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <optional>
#include <vector>

/*
* Shadow caching: a tile's depth image depends only on its view (matrix and square of the atlas) and on the shadow
* casters inside that view. If none of those changed since the tile was last drawn, the image in the atlas is still
* right and drawing it again would only repeat the same work.
*/

// A fingerprint of the casters a tile would draw: each one's mesh, model matrix and shape (a deforming mesh's,
// DrawItem::shape), in order. Any caster moving, changing mesh or shape, or entering or leaving the view changes it
// (barring a one in 2^64 coincidence).
std::uint64_t shadowCasterSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters);

// The same for a tile that also holds its glass in the coloured shadow map (render/gpu/transparency.h): the solid
// casters' signature, then each pane's mesh, matrix, shape and everything its tint comes from (its material's colour,
// coverage, transmission, metalness, thickness, absorption, sides and their textures). Without glass, exactly
// shadowCasterSignature, so tiles without glass are kept as before.
std::uint64_t tintedShadowSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters, const std::vector<std::size_t>& glass);

/*
* Still and moving casters (docs/SHADOWCACHE.md): a tile that keeps them apart (ShadowTile::separateMovers, the far
* shadow map) holds a cached copy of its still casters, and every frame its moving casters' patches are copied back from
* that copy and the movers drawn over them, so things moving in it neither redraw it whole nor wait for a redraw.
*/

// Whether a draw moved since last frame: its matrix is not last frame's. One with no last frame (new, or teleported:
// MotionHistory) has not.
bool movedSinceLastFrame(const DrawItem& draw);

// A tile's casters split into those standing still and those moving this frame, each in the casters' order.
struct ShadowCasterSplit {
    std::vector<std::size_t> still;
    std::vector<std::size_t> moving;
};

ShadowCasterSplit splitMovingCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters);

// A rectangle of the atlas, in texels: where a caster's shadow can be within its tile.
struct ShadowPatch {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/*
* Where a caster with these bounds draws in a tile: its bounding sphere's box seen through the tile's view, rounded out
* to whole texels and a texel more each way (a mover's shadow is in it, whatever the rounding of its triangles), clamped
* to the tile. Rows are counted as the flipped viewport draws them (render/vulkan/vulkan_pipeline.cpp, setViewport): a
* point at clip y lands on row tile.y + (1 - y) / 2 * size. None when the box misses the tile; the whole tile for
* unknown bounds (a negative radius).
*/
std::optional<ShadowPatch> shadowPatch(const ShadowTile& tile, const BoundingSphere& bounds);

// What was last drawn into each square of the atlas.
class ShadowTileMemory {
public:
    // True when this tile's square last received exactly this view of casters with this signature.
    bool isCurrent(const ShadowTile& tile, std::uint64_t signature) const;

    // Record a tile just drawn. Whatever was remembered in squares it overlaps is forgotten, since it drew over them.
    void remember(const ShadowTile& tile, std::uint64_t signature);

    // After an error, or when the atlas's contents are lost.
    void forgetAll();

private:
    struct Entry {
        ShadowTile tile;
        std::uint64_t signature = 0;
    };

    std::vector<Entry> entries;
};
