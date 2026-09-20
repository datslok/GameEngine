#pragma once

#include "pixel.h"

#include <string>

struct Material {
    Pixel colour{255, 255, 255};

    // Empty means plain colour, using the white fallback texture.
    std::string texturePath;
};