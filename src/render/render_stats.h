#pragma once

#include <cstddef>

// What the last frame drew and what it skipped, counting each mesh once per view (each shadow tile is a view).
// Shadow views also skip meshes that do not cast shadows.
struct RenderStats {
    std::size_t drawn = 0;
    std::size_t culled = 0;
    std::size_t shadowDrawn = 0;
    std::size_t shadowCulled = 0;

    // Shadow views drawn, and point light cube faces left out because they see nothing on screen.
    std::size_t shadowTilesDrawn = 0;
    std::size_t shadowTilesSkipped = 0;

    // Point lights in the frame, how many could reach what the camera sees, and how many got a seat in the shader and a shadow.
    std::size_t pointLights = 0;
    std::size_t pointLightsInView = 0;
    std::size_t pointLightsLit = 0;
    std::size_t pointLightsShadowed = 0;
};