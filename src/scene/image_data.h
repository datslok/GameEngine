#pragma once

#include "core/block_compression.h"
#include "core/coverage_mips.h"
#include "scene/asset_handles.h"

#include <cstdint>
#include <vector>

/*
* Whether a texture may be block-compressed for the GPU (core/block_compression.h; docs/TEXTURECOMPRESSION.md). Exact
* is for textures read as exact numbers, whose small errors would show: a game's lookup tables and blend weights.
*/
enum class TextureStorage {
    Compressible,
    Exact,
};

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

    TextureStorage storage = TextureStorage::Compressible;
};

// A texture as BC7 blocks, every mip level from the full size down to one texel (assets/texture_cooking.h).
struct CompressedTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureEncoding encoding = TextureEncoding::Srgb;
    std::vector<CompressedLevel> levels;
};
