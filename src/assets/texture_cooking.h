#pragma once

#include "core/block_compression.h"
#include "core/settings_visitor.h"
#include "scene/image_data.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

class JobSystem;

/*
* Cooking a texture for the GPU (docs/TEXTURECOMPRESSION.md): every mip level built on the CPU and encoded as BC7, a
* quarter of the video memory of RGBA8. Encoding is slow (a search per block), so each result is kept in a file in the
* texture cache, named by a hash of everything that decides its blocks; later starts read the file instead. A game's
* generated textures are seeded, so they are kept too.
*/

// Where the cooked textures are kept, from the folder the game runs in (the repository root, or the packaged demo's).
inline constexpr const char* textureCacheFolder = "build/texture_cache";

/*
* Settings for compressing textures (engine.textureCompression.*). Both are read as each texture reaches the GPU, so a
* change applies to textures loaded after it: in practice at the next start.
*/
struct TextureCompressionSettings {
    // BC7 for every compressible texture; off uploads RGBA8 as before (for comparing).
    bool compress = true;

    // The encoder's effort, 0 to bc7HighestQuality (Bc7Options::quality); part of the cache's names, so a change
    // encodes everything again.
    int quality = 0;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(TextureCompressionSettings& settings, SettingsVisitor& visitor);

// Whether the image may be compressed at all (not a game's exact lookup table).
bool shouldCompress(const ImageData& image);

// The encoder's options for this image: perceptual weights for sRGB colour, plain ones for data such as normal maps.
Bc7Options bc7OptionsFor(const ImageData& image, const TextureCompressionSettings& settings);

/*
* Every level as BC7 blocks: the image's built mips (a cut-out's, keeping coverage) when it has them, else each level
* the one above averaged (core/coverage_mips.h, averagedMips: sRGB as light, normals at their averaged length, which
* Toksvig reads).
*/
CompressedTexture compressTexture(const ImageData& image, const Bc7Options& options, JobSystem& jobs);

// A 64-bit hash of the shape, encoding, texels, built mips, the options and the file format's version.
std::uint64_t textureCacheKey(const ImageData& image, const Bc7Options& options);

// The cache file's name for a key: sixteen hex digits and ".bc7".
std::string textureCacheFileName(std::uint64_t key);

// Write the texture to a file; throws std::runtime_error when it cannot be written.
void writeCompressedTexture(const std::filesystem::path& file, const CompressedTexture& texture);

// The texture in a file, or nothing when the file is missing, cut short, of another format version or inconsistent.
std::optional<CompressedTexture> readCompressedTexture(const std::filesystem::path& file);

/*
* The image as BC7: read from the cache folder when kept there with the image's shape and encoding, else encoded and
* kept (through a temporary file renamed into place, so a half-written file is never read). A folder that cannot be
* made or written leaves the texture encoded but not kept. Throws std::invalid_argument for an image that must stay
* exact.
*/
CompressedTexture cookTexture(const ImageData& image, const TextureCompressionSettings& settings, const std::filesystem::path& folder, JobSystem& jobs);
