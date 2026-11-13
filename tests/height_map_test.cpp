#include "core/height_map.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace {
    // A size x size height map (grey, alpha 255) from a function of the texel's column and row.
    template <typename Height>
    ImageLevel heightImage(std::uint32_t size, Height height) {
        ImageLevel image{size, size, {}};

        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::uint8_t value = height(x, y);
                image.pixels.insert(image.pixels.end(), {value, value, value, 255});
            }
        }

        return image;
    }

    // A texel of a normal map decoded back to a direction, as the shader does: 0..255 to -1..1.
    Vec3 decoded(const ImageLevel& normals, std::uint32_t x, std::uint32_t y) {
        const std::size_t at = (static_cast<std::size_t>(y) * normals.width + x) * 4;
        const auto channel = [&](std::size_t offset) { return normals.pixels[at + offset] / 255.0f * 2.0f - 1.0f; };
        return Vec3{channel(0), channel(1), channel(2)};
    }
}

/*
* A normal map worked out from a height map, so the two always agree (a material's parallax and its lighting): each
* texel's slope by central differences, wrapping round the edges (the textures repeat), its height the byte over 255
* times the depth in texture units (the texture's width being 1), the normal (-dz/du, -dz/dv, 1) made unit length and
* stored as 0..255. Rows are v: row 0 is v = 0, as the engine uploads images.
*/
void testHeightMap() {
    // Flat: straight up, (128, 128, 255) give or take rounding.
    const ImageLevel flat = normalMapFromHeight(heightImage(8, [](std::uint32_t, std::uint32_t) { return std::uint8_t{200}; }), 0.05f);
    assert(flat.width == 8 && flat.height == 8 && flat.pixels.size() == 8 * 8 * 4);
    const Vec3 up = decoded(flat, 3, 5);
    assert(std::fabs(up.x) < 0.01f && std::fabs(up.y) < 0.01f && up.z > 0.99f && flat.pixels[3] == 255);

    // Rising along u (columns): the normal leans back towards -u by the slope's angle. Height x * 10 bytes over a
    // 64-texel picture: dz/du = 10 / 255 * depth * 64.
    const float depth = 0.05f;
    const ImageLevel ramp = normalMapFromHeight(heightImage(64, [](std::uint32_t x, std::uint32_t) { return static_cast<std::uint8_t>(x * 3); }), depth);
    const float slope = 3.0f / 255.0f * depth * 64.0f;
    const Vec3 leaning = decoded(ramp, 20, 7);
    assert(std::fabs(leaning.x - (-slope / std::sqrt(1.0f + slope * slope))) < 0.01f && std::fabs(leaning.y) < 0.01f);

    // Rising along v (rows): leaning towards -v.
    const ImageLevel rows = normalMapFromHeight(heightImage(64, [](std::uint32_t, std::uint32_t y) { return static_cast<std::uint8_t>(y * 3); }), depth);
    assert(std::fabs(decoded(rows, 7, 20).y - (-slope / std::sqrt(1.0f + slope * slope))) < 0.01f && std::fabs(decoded(rows, 7, 20).x) < 0.01f);

    // The edges wrap: column 0's left neighbour is the last column, so a ramp's seam leans the other way, steeply.
    assert(decoded(ramp, 0, 7).x > 0.5f);

    // No depth: flat whatever the heights.
    assert(decoded(normalMapFromHeight(heightImage(16, [](std::uint32_t x, std::uint32_t) { return static_cast<std::uint8_t>(x * 15); }), 0.0f), 5, 5).z > 0.99f);
}
