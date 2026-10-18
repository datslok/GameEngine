#include "core/raw_image.h"

#include <stdexcept>

std::size_t bytesPerPixel(PixelFormat format) {
    return format == PixelFormat::RgbaHalf ? 8 : 4;
}

// The bytes stay in place: only the description of the pixels changes.
RawImage packHalfImage(const RawImage& halfImage) {
    if (halfImage.format != PixelFormat::RgbaHalf) {
        throw std::invalid_argument("packHalfImage needs a half float image");
    }

    RawImage packed = halfImage;
    packed.format = PixelFormat::Rgba8;
    packed.width = halfImage.width * 2;
    return packed;
}

RawImage unpackHalfImage(const RawImage& packed) {
    if (packed.format != PixelFormat::Rgba8 || packed.width % 2 != 0) {
        throw std::invalid_argument("unpackHalfImage needs an 8-bit image of even width");
    }

    RawImage image = packed;
    image.format = PixelFormat::RgbaHalf;
    image.width = packed.width / 2;
    return image;
}
