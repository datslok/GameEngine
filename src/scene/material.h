#pragma once

#include "core/pixel.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Material {
    Pixel colour{255, 255, 255};

    // A texture can come from a file or embedded image bytes.
    // If both are empty, use the white fallback texture.
    std::string texturePath;

    std::shared_ptr<const std::vector<std::uint8_t>> embeddedImage;

    bool flipTextureVertically = true;
};