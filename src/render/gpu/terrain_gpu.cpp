#include "render/gpu/terrain_gpu.h"

#include <cstring>
#include <stdexcept>

GpuTerrainPatch packTerrainPatch(const TerrainPatch& patch) {
    return GpuTerrainPatch{.placement = {patch.corner.x, patch.corner.y, patch.size, patch.quads},
                           .morph = {patch.morphStart, patch.morphEnd, patch.spacing, 0.0f},
                           .uv = {patch.uvCorner.x, patch.uvCorner.y, patch.uvPerMetre, 0.0f}};
}

std::vector<std::uint32_t> packTerrainHeights(const TerrainHeights& heights) {
    if (heights.grids.size() > TERRAIN_MOST_GRIDS) {
        throw std::invalid_argument("A terrain has at most TERRAIN_MOST_GRIDS height grids");
    }

    GpuTerrainHeightsHeader header{};
    header.gridCount = static_cast<std::uint32_t>(heights.grids.size());
    std::uint32_t offset = 0;

    for (std::size_t index = 0; index < heights.grids.size(); ++index) {
        const TerrainHeightGrid& grid = heights.grids[index];
        header.grids[index] = GpuTerrainGrid{.corner = {grid.corner.x, grid.corner.y}, .spacing = grid.spacing, .size = grid.size, .offset = offset,
                                              .inverseSpacing = 1.0f / grid.spacing, .pyramidOffset = 0, .unused = 0};
        offset += static_cast<std::uint32_t>(grid.heights.size());
    }

    for (std::size_t index = 0; index < heights.grids.size(); ++index) {
        header.grids[index].pyramidOffset = offset;
        offset += static_cast<std::uint32_t>(heights.grids[index].pyramid.size());
    }

    std::vector<std::uint32_t> words(sizeof(header) / sizeof(std::uint32_t) + offset);
    std::memcpy(words.data(), &header, sizeof(header));
    std::size_t at = sizeof(header) / sizeof(std::uint32_t);

    for (const TerrainHeightGrid& grid : heights.grids) {
        std::memcpy(&words[at], grid.heights.data(), grid.heights.size() * sizeof(float));
        at += grid.heights.size();
    }

    for (const TerrainHeightGrid& grid : heights.grids) {
        std::memcpy(&words[at], grid.pyramid.data(), grid.pyramid.size() * sizeof(float));
        at += grid.pyramid.size();
    }

    return words;
}
