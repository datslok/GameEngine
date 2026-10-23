#pragma once

#include <cstdint>
#include <vector>

/*
* Mip levels for cut-out textures (glTF's alpha mask: a texel is kept where its alpha reaches a cutoff, else a hole).
* Each smaller level averages four texels, so a leaf or a wire next to a hole averages to a half-clear texel, and a few
* levels down the whole picture sinks under the cutoff: trees go bald and fences vanish with distance. Castano's fix
* (2010, "Computing alpha mipmaps") scales each level's alpha so the share of texels that pass the cutoff (its
* coverage) stays the full-size picture's. Built on the CPU when the texture is loaded; other textures' mips the GPU
* makes by itself.
*/

// One level of an RGBA8 image: four bytes per texel, red, green, blue, alpha, rows top to bottom.
struct ImageLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

// The share of texels a cut-out keeps at this cutoff: alpha (as byte / 255) at or above it, as the shader tests it.
float alphaCoverage(const ImageLevel& level, float cutoff);

/*
* The next mip level: half the size, rounded down, at least one texel, as the GPU sizes them; each texel the mean of
* its 2 x 2 block, an odd last row or column folded into its neighbour's. sRGB colour is averaged as light (decoded,
* averaged, encoded), alpha always as a plain share.
*/
ImageLevel halveImage(const ImageLevel& level, bool srgb);

// Every level below the full size, down to one texel, each with its alpha scaled to keep the full size's coverage.
std::vector<ImageLevel> coveragePreservingMips(const ImageLevel& full, bool srgb, float cutoff);
