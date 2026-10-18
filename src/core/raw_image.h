#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// How a RawImage's bytes are laid out per pixel.
enum class PixelFormat {
    Rgba8,   // four bytes: red, green, blue, alpha
    RgbaHalf // four half floats (core/half_float.h), eight bytes, little-endian as GPUs store them
};

std::size_t bytesPerPixel(PixelFormat format);

/*
* An image exactly as the GPU stored it, rows top to bottom: what the renderer reads back for the golden image check.
* Nothing is converted, so two captures can be compared bit for bit.
*/
struct RawImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::Rgba8;
    std::vector<std::uint8_t> bytes;
};

/*
* A float image as an ordinary 8-bit one, for saving as PNG without a float image library: each pixel's eight bytes
* become two RGBA pixels side by side, so the result is twice as wide and holds exactly the same bytes. PNG is lossless,
* so unpacking a loaded PNG gives back every bit. Throws if the image is not RgbaHalf (or, unpacking, not Rgba8 of even width).
*/
RawImage packHalfImage(const RawImage& halfImage);
RawImage unpackHalfImage(const RawImage& packed);
