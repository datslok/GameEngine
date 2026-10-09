#pragma once

#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <vector>

/*
* Shadow caching: a tile's depth image depends only on its view (matrix and square of the atlas) and on the shadow
* casters inside that view. If none of those changed since the tile was last drawn, the image in the atlas is still
* right and drawing it again would only repeat the same work.
*/

// A fingerprint of the casters a tile would draw: each one's mesh and model matrix, in order. Any caster moving,
// changing mesh, or entering or leaving the view changes it (barring a one in 2^64 coincidence).
std::uint64_t shadowCasterSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters);

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
