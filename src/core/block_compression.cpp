#include "core/block_compression.h"
#include "core/job_system.h"

#include "bc7enc/bc7decomp.h"
#include "bc7enc/bc7enc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace {
    constexpr std::uint32_t blockSize = 4;

    std::uint32_t blocksAcross(std::uint32_t texels) {
        return (texels + blockSize - 1) / blockSize;
    }

    // bc7enc fills its tables of best endpoints once; afterwards blocks may be encoded on any number of threads.
    void startEncoder() {
        static std::once_flag started;
        std::call_once(started, [] { bc7enc_compress_block_init(); });
    }

    bc7enc_compress_block_params encoderParameters(const Bc7Options& options) {
        if (options.quality < 0 || options.quality > bc7HighestQuality) {
            throw std::invalid_argument("BC7 quality must be from 0 to bc7HighestQuality");
        }

        bc7enc_compress_block_params parameters;
        bc7enc_compress_block_params_init(&parameters);
        parameters.m_uber_level = static_cast<std::uint32_t>(options.quality);

        if (!options.perceptual) {
            bc7enc_compress_block_params_init_linear_weights(&parameters);
        }

        return parameters;
    }
}

std::size_t bc7LevelBytes(std::uint32_t width, std::uint32_t height) {
    return std::size_t{blocksAcross(width)} * blocksAcross(height) * bc7BlockBytes;
}

CompressedLevel compressBc7(const ImageLevel& level, const Bc7Options& options, JobSystem& jobs) {
    if (level.width == 0 || level.height == 0 || level.pixels.size() != std::size_t{level.width} * level.height * 4) {
        throw std::invalid_argument("A level to compress needs four bytes for each of its texels");
    }

    startEncoder();
    const bc7enc_compress_block_params parameters = encoderParameters(options);
    const std::uint32_t columns = blocksAcross(level.width);
    const std::uint32_t rows = blocksAcross(level.height);
    CompressedLevel compressed{level.width, level.height, std::vector<std::uint8_t>(bc7LevelBytes(level.width, level.height))};

    jobs.parallelFor(rows, 1, [&](std::size_t row) {
        std::array<std::uint8_t, blockSize * blockSize * 4> texels{};

        for (std::uint32_t column = 0; column < columns; ++column) {
            for (std::uint32_t y = 0; y < blockSize; ++y) {
                const std::uint32_t sourceY = std::min(static_cast<std::uint32_t>(row) * blockSize + y, level.height - 1);

                for (std::uint32_t x = 0; x < blockSize; ++x) {
                    const std::uint32_t sourceX = std::min(column * blockSize + x, level.width - 1);
                    std::memcpy(&texels[(y * blockSize + x) * 4], &level.pixels[(std::size_t{sourceY} * level.width + sourceX) * 4], 4);
                }
            }

            bc7enc_compress_block(&compressed.blocks[(row * columns + column) * bc7BlockBytes], texels.data(), &parameters);
        }
    });

    return compressed;
}

ImageLevel decompressBc7(const CompressedLevel& level) {
    if (level.blocks.size() != bc7LevelBytes(level.width, level.height)) {
        throw std::invalid_argument("A compressed level's blocks do not match its size");
    }

    const std::uint32_t columns = blocksAcross(level.width);
    ImageLevel decoded{level.width, level.height, std::vector<std::uint8_t>(std::size_t{level.width} * level.height * 4)};
    std::array<bc7decomp::color_rgba, blockSize * blockSize> texels;

    for (std::uint32_t row = 0; row < blocksAcross(level.height); ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            if (!bc7decomp::unpack_bc7(&level.blocks[(std::size_t{row} * columns + column) * bc7BlockBytes], texels.data())) {
                throw std::runtime_error("A BC7 block uses no valid mode");
            }

            for (std::uint32_t y = 0; y < blockSize && row * blockSize + y < level.height; ++y) {
                for (std::uint32_t x = 0; x < blockSize && column * blockSize + x < level.width; ++x) {
                    const std::size_t target = (std::size_t{row * blockSize + y} * level.width + column * blockSize + x) * 4;
                    std::memcpy(&decoded.pixels[target], texels[y * blockSize + x].m_comps, 4);
                }
            }
        }
    }

    return decoded;
}
