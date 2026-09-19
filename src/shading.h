#pragma once

#include "pixel.h"
#include "vec3.h"

// normal and toLight must use the same coordinate space.
// toLight points from the surface toward the light.
Pixel shadeFlat(
    Pixel baseColour,
    const Vec3& normal,
    const Vec3& toLight,
    float ambient
);