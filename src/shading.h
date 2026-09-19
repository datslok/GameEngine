#pragma once

#include "pixel.h"
#include "vec3.h"

/*
* The normal and light direction must use the same coordinate space.
* The light direction points from the surface toward the light source.
*/
Pixel shadeFlat(
    Pixel baseColour,
    const Vec3& normal,
    const Vec3& toLight,
    float ambient
);