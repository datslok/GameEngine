#include "core/height_map.h"

#include <cmath>
#include <cstddef>

/*
* Between the neighbours either side lie two texels, each 1 / width of the texture, so the slope along u is
* (right - left) / 255 * depth / (2 / width); likewise along v with the rows.
*/
ImageLevel normalMapFromHeight(const ImageLevel& height, float depth) {
    ImageLevel normals{height.width, height.height, {}};
    normals.pixels.reserve(static_cast<std::size_t>(height.width) * height.height * 4);

    const auto heightAt = [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<float>(height.pixels[(static_cast<std::size_t>(y) * height.width + x) * 4]) / 255.0f;
    };

    const auto encode = [](float component) {
        return static_cast<std::uint8_t>(std::lround((component * 0.5f + 0.5f) * 255.0f));
    };

    for (std::uint32_t y = 0; y < height.height; ++y) {
        for (std::uint32_t x = 0; x < height.width; ++x) {
            const std::uint32_t left = (x + height.width - 1) % height.width;
            const std::uint32_t right = (x + 1) % height.width;
            const std::uint32_t below = (y + height.height - 1) % height.height;
            const std::uint32_t above = (y + 1) % height.height;
            const float alongU = (heightAt(right, y) - heightAt(left, y)) * depth * static_cast<float>(height.width) * 0.5f;
            const float alongV = (heightAt(x, above) - heightAt(x, below)) * depth * static_cast<float>(height.height) * 0.5f;
            const Vec3 normal = Vec3{-alongU, -alongV, 1.0f}.normalized();
            normals.pixels.insert(normals.pixels.end(), {encode(normal.x), encode(normal.y), encode(normal.z), 255});
        }
    }

    return normals;
}
