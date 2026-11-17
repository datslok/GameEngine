#pragma once

#include "core/coverage_mips.h"
#include "scene/asset_handles.h"

#include <cstdint>
#include <vector>

// An image in memory, as loaded (assets/image_loader) or made by the engine (impostors, a game's generated textures).
// Here rather than beside the loader so scene/ can use images without depending on assets/ (docs/LAYOUT.md).
struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Four bytes per pixel: red, green, blue, alpha.
    std::vector<std::uint8_t> pixels;

    // Whether the bytes are sRGB colour or plain numbers, which decides the GPU format they are uploaded in.
    TextureEncoding encoding = TextureEncoding::Srgb;

    // The smaller mip levels, when built on the CPU (a cut-out's, keeping its coverage: core/coverage_mips.h). Empty:
    // the GPU makes them by averaging.
    std::vector<ImageLevel> mips{};
};
