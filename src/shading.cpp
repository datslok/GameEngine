#include "shading.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/*
* Apply flat diffuse lighting to a colour so the rendered surface reflects
* its orientation relative to the light source while retaining ambient light.
*/
Pixel shadeFlat(
    Pixel baseColour,
    const Vec3& normal,
    const Vec3& toLight,
    float ambient
){
    const Vec3 unitNormal = normal.normalized();
    const Vec3 unitLight = toLight.normalized();

    const float diffuse = std::clamp(
        unitNormal.dot(unitLight),
        0.0f,
        1.0f
    );

    ambient = std::clamp(ambient, 0.0f, 1.0f);

    const float brightness =
        ambient + (1.0f - ambient) * diffuse;

    return Pixel{
        static_cast<std::uint8_t>(
            std::lround(baseColour.r * brightness)
        ),
        static_cast<std::uint8_t>(
            std::lround(baseColour.g * brightness)
        ),
        static_cast<std::uint8_t>(
            std::lround(baseColour.b * brightness)
        )
    };
}