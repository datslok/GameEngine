#pragma once

#include <cstdint>

// How many mip levels a full chain has for this size: the original plus each halving until the long side is one pixel.
std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height);
