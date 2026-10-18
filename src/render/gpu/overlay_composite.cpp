#include "render/gpu/overlay_composite.h"
#include "core/srgb.h"

#include <algorithm>

/*
* An empty texel (no coverage) writes nothing, which also keeps the division by alpha safe. Partial coverage (the soft
* edges of letters) is decoded from its own colour: decoding the premultiplied value instead would darken every edge.
* Dividing an 8-bit value by an 8-bit alpha can round past 1, so the colour is capped there.
*/
Vec4 overlayOutput(const Vec4& texel, float whiteLevel) {
    const float alpha = texel.w;

    if (!(alpha > 0.0f)) {
        return Vec4{0.0f, 0.0f, 0.0f, 0.0f};
    }

    const float scale = alpha * whiteLevel;
    return Vec4{
        srgbToLinear(std::min(texel.x / alpha, 1.0f)) * scale,
        srgbToLinear(std::min(texel.y / alpha, 1.0f)) * scale,
        srgbToLinear(std::min(texel.z / alpha, 1.0f)) * scale,
        alpha
    };
}

Vec3 blendOverlay(const Vec3& screen, const Vec4& output) {
    const float keep = 1.0f - output.w;
    return Vec3{output.x + screen.x * keep, output.y + screen.y * keep, output.z + screen.z * keep};
}
