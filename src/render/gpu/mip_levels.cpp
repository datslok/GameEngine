#include "render/gpu/mip_levels.h"

#include <algorithm>
#include <stdexcept>

/*
* Counting halvings directly (instead of floor(log2) in floating point) is exact for every size.
* Each level rounds down, so a 300 pixel side goes 300, 150, 75, 37, 18, 9, 4, 2, 1.
*/
std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        throw std::invalid_argument("Mip levels need a texture of at least one pixel");
    }

    std::uint32_t longSide = std::max(width, height);
    std::uint32_t levels = 1;

    while (longSide > 1) {
        longSide /= 2;
        ++levels;
    }

    return levels;
}
