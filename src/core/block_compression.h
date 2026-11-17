#pragma once

#include "core/coverage_mips.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class JobSystem;

/*
* BC7 block compression (docs/research/TEXTURECOMPRESSION.md): an RGBA picture stored as 4 x 4 texel blocks of 16 bytes, one
* byte a texel against four, which the GPU decodes as it samples, so textures take a quarter of the video memory. Each
* block holds one to three pairs of endpoint colours and, per texel, where it lies on the line between its pair: the
* encoder searches for the endpoints and indices that miss the original least. The search is bc7enc's
* (external/bc7enc, Rich Geldreich); the GPU, or decompressBc7 for tests, does the decoding.
*/

// The bytes of one BC7 block (4 x 4 texels).
inline constexpr std::size_t bc7BlockBytes = 16;

// Most effort the encoder takes (bc7enc's uber level).
inline constexpr int bc7HighestQuality = 4;

struct Bc7Options {
    // 0 to bc7HighestQuality: higher searches more endpoints, slower to encode, slightly closer. Decoding costs the same.
    int quality = 0;

    // Weigh errors as the eye sees colour (bc7enc's YCbCr weights, brightness most): for sRGB colour. Off weighs red,
    // green, blue and alpha alike: for data such as normal maps, whose channels are numbers, not colour.
    bool perceptual = true;
};

// One mip level as BC7 blocks, row after row of blocks, top first; the texel size as the level's own (not rounded up).
struct CompressedLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> blocks;
};

// The bytes a level of this size takes: a block for every 4 x 4 texels, a part-filled block at an odd edge counted whole.
std::size_t bc7LevelBytes(std::uint32_t width, std::uint32_t height);

/*
* The level as BC7 blocks, a row of blocks a job. Blocks past the picture's right or bottom edge are filled by repeating
* its last column or row, so the edge's texels are fitted as well as any. The same blocks on any number of threads.
*/
CompressedLevel compressBc7(const ImageLevel& level, const Bc7Options& options, JobSystem& jobs);

// The texels back from the blocks, as the GPU decodes them (bit-exact: BC7's decoding is fully specified).
ImageLevel decompressBc7(const CompressedLevel& level);
