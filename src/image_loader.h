#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Four bytes per pixel: red, green, blue, alpha.
    std::vector<std::uint8_t> pixels;
};

// Flip rows by default to use bottom-left UV coordinates.
ImageData loadImage(
    const std::string& filename,
    bool flipVertically = true
);