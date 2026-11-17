#include "assets/texture_cooking.h"
#include "core/hash.h"
#include "core/job_system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {
    // The file's first bytes, and its layout's version: a change to the layout or to how blocks are made (the encoder,
    // the mips) bumps it, so old files are no longer found (the version is part of the key too).
    constexpr std::array<char, 4> fileMagic{'G', 'E', 'B', '7'};
    constexpr std::uint32_t fileVersion = 1;

    // The levels from the full size down to one texel, as the GPU counts them.
    std::size_t fullChainLength(std::uint32_t width, std::uint32_t height) {
        std::size_t levels = 1;

        while (width > 1 || height > 1) {
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
            ++levels;
        }

        return levels;
    }

    template <typename Number>
    void writeNumber(std::ofstream& out, Number number) {
        out.write(reinterpret_cast<const char*>(&number), sizeof(number));
    }

    template <typename Number>
    bool readNumber(std::ifstream& in, Number& number) {
        return static_cast<bool>(in.read(reinterpret_cast<char*>(&number), sizeof(number)));
    }

    // Each temporary file gets its own number, so two jobs keeping the same texture never write one file at once.
    std::atomic<std::uint64_t> temporaryFiles{0};
}

void visit(TextureCompressionSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Compress", settings.compress, FieldApplies::AtStartup);
    visitor.field("Quality", settings.quality, {.min = 0, .max = bc7HighestQuality, .applies = FieldApplies::AtStartup});
}

bool shouldCompress(const ImageData& image) {
    return image.storage == TextureStorage::Compressible;
}

Bc7Options bc7OptionsFor(const ImageData& image, const TextureCompressionSettings& settings) {
    return Bc7Options{.quality = settings.quality, .perceptual = image.encoding == TextureEncoding::Srgb};
}

CompressedTexture compressTexture(const ImageData& image, const Bc7Options& options, JobSystem& jobs) {
    const ImageLevel full{image.width, image.height, image.pixels};
    const std::vector<ImageLevel> averaged = image.mips.empty() ? averagedMips(full, image.encoding == TextureEncoding::Srgb) : std::vector<ImageLevel>{};
    const std::vector<ImageLevel>& smaller = image.mips.empty() ? averaged : image.mips;

    if (smaller.size() + 1 != fullChainLength(image.width, image.height)) {
        throw std::invalid_argument("A texture's built mip levels must be every level below its full size");
    }

    CompressedTexture compressed{image.width, image.height, image.encoding, {}};
    compressed.levels.push_back(compressBc7(full, options, jobs));

    for (const ImageLevel& level : smaller) {
        compressed.levels.push_back(compressBc7(level, options, jobs));
    }

    return compressed;
}

/*
* Field by field (core/hash.h: never a struct's padding). Hashing the texels is most of the work, small beside encoding
* them.
*/
std::uint64_t textureCacheKey(const ImageData& image, const Bc7Options& options) {
    Fnv1aHash hash;
    hash.add(fileVersion);
    hash.add(image.width);
    hash.add(image.height);
    hash.add(static_cast<std::uint32_t>(image.encoding));
    hash.add(options.quality);
    hash.add(options.perceptual);
    hash.addBytes(image.pixels.data(), image.pixels.size());
    hash.add(static_cast<std::uint64_t>(image.mips.size()));

    for (const ImageLevel& level : image.mips) {
        hash.add(level.width);
        hash.add(level.height);
        hash.addBytes(level.pixels.data(), level.pixels.size());
    }

    return hash.value;
}

std::string textureCacheFileName(std::uint64_t key) {
    std::array<char, 17> digits{};
    std::snprintf(digits.data(), digits.size(), "%016llx", static_cast<unsigned long long>(key));
    return std::string{digits.data()} + ".bc7";
}

/*
* The magic and version, the size, encoding and level count, then each level's size, byte count and blocks. Numbers in
* the machine's own byte order: the cache is never shared between machines.
*/
void writeCompressedTexture(const std::filesystem::path& file, const CompressedTexture& texture) {
    std::ofstream out{file, std::ios::binary | std::ios::trunc};

    if (!out) {
        throw std::runtime_error("Cannot write the cooked texture " + file.string());
    }

    out.write(fileMagic.data(), fileMagic.size());
    writeNumber(out, fileVersion);
    writeNumber(out, texture.width);
    writeNumber(out, texture.height);
    writeNumber(out, static_cast<std::uint32_t>(texture.encoding));
    writeNumber(out, static_cast<std::uint32_t>(texture.levels.size()));

    for (const CompressedLevel& level : texture.levels) {
        writeNumber(out, level.width);
        writeNumber(out, level.height);
        writeNumber(out, static_cast<std::uint64_t>(level.blocks.size()));
        out.write(reinterpret_cast<const char*>(level.blocks.data()), static_cast<std::streamsize>(level.blocks.size()));
    }

    if (!out.flush()) {
        throw std::runtime_error("Cannot write the cooked texture " + file.string());
    }
}

/*
* Everything is checked before it is trusted: each level must have its place in the chain's size and exactly its
* blocks, and the file must end there.
*/
std::optional<CompressedTexture> readCompressedTexture(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};

    if (!in) {
        return std::nullopt;
    }

    std::array<char, 4> magic{};
    std::uint32_t version = 0;
    std::uint32_t encoding = 0;
    std::uint32_t levelCount = 0;
    CompressedTexture texture;

    if (!in.read(magic.data(), magic.size()) || magic != fileMagic || !readNumber(in, version) || version != fileVersion ||
        !readNumber(in, texture.width) || !readNumber(in, texture.height) || !readNumber(in, encoding) || !readNumber(in, levelCount)) {
        return std::nullopt;
    }

    if (texture.width == 0 || texture.height == 0 || encoding > static_cast<std::uint32_t>(TextureEncoding::Linear) ||
        levelCount != fullChainLength(texture.width, texture.height)) {
        return std::nullopt;
    }

    texture.encoding = static_cast<TextureEncoding>(encoding);

    for (std::uint32_t index = 0; index < levelCount; ++index) {
        CompressedLevel level;
        std::uint64_t byteCount = 0;

        if (!readNumber(in, level.width) || !readNumber(in, level.height) || !readNumber(in, byteCount) ||
            level.width != std::max(texture.width >> index, 1u) || level.height != std::max(texture.height >> index, 1u) ||
            byteCount != bc7LevelBytes(level.width, level.height)) {
            return std::nullopt;
        }

        level.blocks.resize(byteCount);

        if (!in.read(reinterpret_cast<char*>(level.blocks.data()), static_cast<std::streamsize>(byteCount))) {
            return std::nullopt;
        }

        texture.levels.push_back(std::move(level));
    }

    if (in.peek() != std::ifstream::traits_type::eof()) {
        return std::nullopt;
    }

    return texture;
}

CompressedTexture cookTexture(const ImageData& image, const TextureCompressionSettings& settings, const std::filesystem::path& folder, JobSystem& jobs) {
    if (!shouldCompress(image)) {
        throw std::invalid_argument("This texture must stay exact, not compressed");
    }

    const Bc7Options options = bc7OptionsFor(image, settings);
    const std::filesystem::path file = folder / textureCacheFileName(textureCacheKey(image, options));

    if (std::optional<CompressedTexture> kept = readCompressedTexture(file);
        kept && kept->width == image.width && kept->height == image.height && kept->encoding == image.encoding) {
        return std::move(*kept);
    }

    CompressedTexture compressed = compressTexture(image, options, jobs);

    // Keeping it is a saving for next time, not needed now: a cache that cannot be written is skipped.
    try {
        std::filesystem::create_directories(folder);
        std::filesystem::path temporary = file;
        temporary += ".writing" + std::to_string(temporaryFiles.fetch_add(1));
        writeCompressedTexture(temporary, compressed);
        std::filesystem::rename(temporary, file);
    } catch (const std::exception&) {
        // Left unkept; the next start encodes it again.
    }

    return compressed;
}
