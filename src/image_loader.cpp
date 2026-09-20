#include "image_loader.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
    // Tell unique_ptr how to release an SDL surface.
    struct SurfaceDeleter {
        void operator()(SDL_Surface* surface) const noexcept {
            SDL_DestroySurface(surface);
        }
    };

    std::runtime_error imageError(
        const std::string& message,
        const std::string& filename
    ) {
        return std::runtime_error(
            message + " '" + filename + "': " + SDL_GetError()
        );
    }
}

ImageData loadImage(
    const std::string& filename,
    bool flipVertically
) {
    std::unique_ptr<SDL_Surface, SurfaceDeleter> original{
        IMG_Load(filename.c_str())
    };

    if (!original) {
        throw imageError("Could not load image", filename);
    }

    // RGBA32 guarantees R, G, B, A byte order in memory.
    std::unique_ptr<SDL_Surface, SurfaceDeleter> converted{
        SDL_ConvertSurface(original.get(), SDL_PIXELFORMAT_RGBA32)
    };

    if (!converted) {
        throw imageError("Could not convert image", filename);
    }

    if (converted->w <= 0 || converted->h <= 0) {
        throw std::runtime_error(
            "Image dimensions must be positive: " + filename
        );
    }

    ImageData image;
    image.width = static_cast<std::uint32_t>(converted->w);
    image.height = static_cast<std::uint32_t>(converted->h);

    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(image.width) * image.height;

    // Match the upload size limit in GpuTexture.
    if (pixelCount > std::numeric_limits<std::uint32_t>::max() / 4) {
        throw std::overflow_error(
            "Image is too large for GPU upload: " + filename
        );
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(image.width) * 4;

    image.pixels.resize(static_cast<std::size_t>(pixelCount * 4));

    if (!SDL_LockSurface(converted.get())) {
        throw imageError("Could not lock image pixels", filename);
    }

    const std::uint8_t* source =
        static_cast<const std::uint8_t*>(converted->pixels);

    // SDL's pitch is the byte distance between rows.
    // It can include padding, so copy each row separately.
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::uint32_t sourceY =
            flipVertically ? image.height - 1 - y : y;

        const std::size_t sourceOffset =
            static_cast<std::size_t>(sourceY) *
            static_cast<std::size_t>(converted->pitch);

        const std::size_t destinationOffset =
            static_cast<std::size_t>(y) * rowBytes;

        std::memcpy(
            image.pixels.data() + destinationOffset,
            source + sourceOffset,
            rowBytes
        );
    }

    SDL_UnlockSurface(converted.get());

    // Both SDL surfaces are released automatically by unique_ptr.
    return image;
}