#pragma once

#include <cstddef>

// What the last frame drew and what it skipped, counting each mesh once per view (each shadow tile is a view).
// Shadow views also skip meshes that do not cast shadows.
struct RenderStats {
    std::size_t drawn = 0;
    std::size_t culled = 0;
    std::size_t shadowDrawn = 0;
    std::size_t shadowCulled = 0;
};