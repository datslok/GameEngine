#include "assets/texture_cooking.h"
#include "core/block_compression.h"
#include "core/coverage_mips.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
    ImageData stripedImage(std::uint32_t width, std::uint32_t height, TextureEncoding encoding) {
        ImageData image{width, height, {}, encoding};

        for (std::uint32_t texel = 0; texel < width * height; ++texel) {
            const auto shade = static_cast<std::uint8_t>((texel % width) * 8);
            image.pixels.insert(image.pixels.end(), {shade, static_cast<std::uint8_t>(255 - shade), 60, 255});
        }

        return image;
    }

    // A folder of its own under the system's temp folder, emptied before and after.
    struct TemporaryFolder {
        std::filesystem::path path;

        explicit TemporaryFolder(const char* name):
            path(std::filesystem::temp_directory_path() / name)
        {
            std::filesystem::remove_all(path);
        }

        ~TemporaryFolder() {
            std::filesystem::remove_all(path);
        }
    };

    // Without built mips, every level down to one texel is made on the CPU (the GPU cannot write blocks), each the
    // averaged level above; built ones (a cut-out's) are used as they are.
    void testCompressionMakesEveryLevel() {
        const ImageData image = stripedImage(16, 8, TextureEncoding::Srgb);
        const CompressedTexture compressed = compressTexture(image, Bc7Options{}, testJobs());
        assert(compressed.width == 16 && compressed.height == 8 && compressed.encoding == TextureEncoding::Srgb);
        assert(compressed.levels.size() == 5);
        assert(compressed.levels[1].width == 8 && compressed.levels[1].height == 4);
        assert(compressed.levels[4].width == 1 && compressed.levels[4].height == 1);

        for (const CompressedLevel& level : compressed.levels) {
            assert(level.blocks.size() == bc7LevelBytes(level.width, level.height));
        }

        ImageData cutOut = stripedImage(8, 8, TextureEncoding::Srgb);
        cutOut.mips = averagedMips(ImageLevel{8, 8, cutOut.pixels}, true);

        for (ImageLevel& level : cutOut.mips) {
            for (std::size_t alpha = 3; alpha < level.pixels.size(); alpha += 4) {
                level.pixels[alpha] = 0;
            }
        }

        const CompressedTexture withBuilt = compressTexture(cutOut, Bc7Options{}, testJobs());
        assert(withBuilt.levels.size() == 4);
        assert(decompressBc7(withBuilt.levels[0]).pixels[3] >= 254); // see block_compression_test: solid may come back as 254
        assert(decompressBc7(withBuilt.levels[1]).pixels[3] <= 1);
    }

    // Anything that changes the blocks changes the key: the texels, the shape, the encoding, built mips and the
    // encoder's options. The same input gives the same key.
    void testCacheKeys() {
        const ImageData image = stripedImage(8, 4, TextureEncoding::Srgb);
        const Bc7Options options{};
        const std::uint64_t key = textureCacheKey(image, options);
        assert(textureCacheKey(stripedImage(8, 4, TextureEncoding::Srgb), options) == key);

        ImageData texel = image;
        texel.pixels[5] ^= 1;
        assert(textureCacheKey(texel, options) != key);

        ImageData shape = image;
        shape.width = 4;
        shape.height = 8;
        assert(textureCacheKey(shape, options) != key);

        ImageData encoding = image;
        encoding.encoding = TextureEncoding::Linear;
        assert(textureCacheKey(encoding, options) != key);

        ImageData mips = image;
        mips.mips = averagedMips(ImageLevel{8, 4, image.pixels}, true);
        assert(textureCacheKey(mips, options) != key);

        assert(textureCacheKey(image, Bc7Options{.quality = options.quality + 1}) != key);
        assert(textureCacheKey(image, Bc7Options{.perceptual = !options.perceptual}) != key);
    }

    // A file written is read back as it was; a missing, cut short or foreign file reads as nothing (encoded again).
    void testFilesRoundTrip() {
        const TemporaryFolder folder{"game_engine_texture_files"};
        std::filesystem::create_directories(folder.path);
        const CompressedTexture compressed = compressTexture(stripedImage(12, 6, TextureEncoding::Linear), Bc7Options{}, testJobs());
        const std::filesystem::path file = folder.path / "one.bc7";
        writeCompressedTexture(file, compressed);

        const std::optional<CompressedTexture> read = readCompressedTexture(file);
        assert(read && read->width == 12 && read->height == 6 && read->encoding == TextureEncoding::Linear);
        assert(read->levels.size() == compressed.levels.size());

        for (std::size_t level = 0; level < compressed.levels.size(); ++level) {
            assert(read->levels[level].width == compressed.levels[level].width && read->levels[level].blocks == compressed.levels[level].blocks);
        }

        assert(!readCompressedTexture(folder.path / "missing.bc7"));

        const auto size = std::filesystem::file_size(file);
        std::filesystem::resize_file(file, size - 5);
        assert(!readCompressedTexture(file));

        std::ofstream{folder.path / "foreign.bc7", std::ios::binary} << "not a texture at all, just some words";
        assert(!readCompressedTexture(folder.path / "foreign.bc7"));
    }

    // The first cook encodes and keeps a file named by the key; the next reads that file instead of encoding. A folder
    // that cannot be made leaves the texture encoded but not kept.
    void testCookUsesTheCache() {
        const TemporaryFolder folder{"game_engine_texture_cache"};
        const TextureCompressionSettings settings{};
        const ImageData image = stripedImage(8, 8, TextureEncoding::Srgb);
        const CompressedTexture first = cookTexture(image, settings, folder.path, testJobs());
        const std::filesystem::path file = folder.path / textureCacheFileName(textureCacheKey(image, bc7OptionsFor(image, settings)));
        assert(std::filesystem::exists(file));

        // Swap the kept file for another texture's blocks of the same shape: the next cook returns those, so it read the file.
        ImageData different = image;
        std::fill(different.pixels.begin(), different.pixels.end(), std::uint8_t{200});
        const CompressedTexture other = compressTexture(different, Bc7Options{}, testJobs());
        assert(other.levels[0].blocks != first.levels[0].blocks);
        writeCompressedTexture(file, other);
        assert(cookTexture(image, settings, folder.path, testJobs()).levels[0].blocks == other.levels[0].blocks);

        // A kept file of another shape or encoding (a key collision) is not trusted.
        writeCompressedTexture(file, compressTexture(stripedImage(4, 4, TextureEncoding::Srgb), Bc7Options{}, testJobs()));
        assert(cookTexture(image, settings, folder.path, testJobs()).width == 8);

        std::ofstream{folder.path / "blocked", std::ios::binary} << "a file where a folder should be";
        assert(cookTexture(image, settings, folder.path / "blocked" / "cache", testJobs()).levels.size() == 4);
    }

    // Colour is encoded perceptually (errors weighed as the eye sees them), data as plain numbers; exact textures (a
    // game's lookup tables) are never compressed.
    void testChoices() {
        const TextureCompressionSettings settings{};
        ImageData colour = stripedImage(4, 4, TextureEncoding::Srgb);
        ImageData data = stripedImage(4, 4, TextureEncoding::Linear);
        assert(bc7OptionsFor(colour, settings).perceptual);
        assert(!bc7OptionsFor(data, settings).perceptual);
        assert(bc7OptionsFor(colour, settings).quality == settings.quality);
        assert(shouldCompress(colour) && shouldCompress(data));

        data.storage = TextureStorage::Exact;
        assert(!shouldCompress(data));

        bool threw = false;
        try {
            cookTexture(data, settings, std::filesystem::temp_directory_path(), testJobs());
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}

void testTextureCooking() {
    testCompressionMakesEveryLevel();
    testCacheKeys();
    testFilesRoundTrip();
    testCookUsesTheCache();
    testChoices();
}
