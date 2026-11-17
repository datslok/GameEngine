#pragma once

#include "core/raw_image.h"
#include "scene/image_data.h"

#include <cstdint>
#include <string>
#include <vector>
#include <span>

// Flip rows by default to use bottom-left UV coordinates.
ImageData loadImage(
    const std::string& filename,
    bool flipVertically = true
);

ImageData loadImageFromMemory(
    std::span<const std::uint8_t> bytes,
    bool flipVertically = true
);

// An 8-bit image saved as PNG exactly as given (rows top to bottom, no flip) and read back the same way. PNG is lossless,
// so every byte survives, alpha included: for golden images (engine/golden.h). Throw on failure.
void saveRawImagePng(const RawImage& image, const std::string& filename);
RawImage loadRawImagePng(const std::string& filename);
