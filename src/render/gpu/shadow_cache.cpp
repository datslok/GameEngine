#include "render/gpu/shadow_cache.h"
#include "core/hash.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace {
    bool sameMatrix(const Mat4& a, const Mat4& b) {
        return std::memcmp(a.values, b.values, sizeof(a.values)) == 0;
    }

    bool overlap(const ShadowTile& a, const ShadowTile& b) {
        return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
    }
}

/*
* The shadow pass draws only positions, so only the mesh and where it is matter, not its material. The count goes in
* too, so a list cannot match a longer one that starts the same way.
*/
std::uint64_t shadowCasterSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters) {
    Fnv1aHash hash;
    hash.add(static_cast<std::uint64_t>(casters.size()));

    for (std::size_t index : casters) {
        const DrawItem& draw = draws[index];
        hash.add(draw.mesh.index);
        hash.addBytes(draw.model.values, sizeof(draw.model.values));
        hash.add(draw.shape);
    }

    return hash.value;
}

std::uint64_t tintedShadowSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters, const std::vector<std::size_t>& glass) {
    const std::uint64_t solid = shadowCasterSignature(draws, casters);

    if (glass.empty()) {
        return solid;
    }

    Fnv1aHash hash;
    hash.add(solid);
    hash.add(static_cast<std::uint64_t>(glass.size()));

    for (std::size_t index : glass) {
        const DrawItem& draw = draws[index];
        const Material& material = draw.material;
        hash.add(draw.mesh.index);
        hash.addBytes(draw.model.values, sizeof(draw.model.values));
        hash.add(draw.shape);
        hash.add(material.colour.r);
        hash.add(material.colour.g);
        hash.add(material.colour.b);
        hash.add(material.texture.index);
        hash.add(material.metallic);
        hash.add(material.metallicRoughnessTexture.index);
        hash.add(static_cast<int>(material.alphaMode));
        hash.add(material.opacity);
        hash.add(material.doubleSided);
        hash.add(material.transmission);
        hash.add(material.transmissionTexture.index);
        hash.add(material.thickness);
        hash.add(material.thicknessTexture.index);
        hash.add(material.attenuationColour.x);
        hash.add(material.attenuationColour.y);
        hash.add(material.attenuationColour.z);
        hash.add(material.attenuationDistance);
    }

    return hash.value;
}

bool movedSinceLastFrame(const DrawItem& draw) {
    return draw.previousModel.has_value() && !(*draw.previousModel == draw.model);
}

ShadowCasterSplit splitMovingCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters) {
    ShadowCasterSplit split;

    for (std::size_t index : casters) {
        (movedSinceLastFrame(draws[index]) ? split.moving : split.still).push_back(index);
    }

    return split;
}

/*
* The sphere's box (its eight corners) through the tile's view: exact for the sun's orthographic tiles, and holding the
* sphere for any view, since the sphere is inside the box. Measured in texels from the tile's corner.
*/
std::optional<ShadowPatch> shadowPatch(const ShadowTile& tile, const BoundingSphere& bounds) {
    if (bounds.radius < 0.0f) {
        return ShadowPatch{tile.x, tile.y, tile.size, tile.size};
    }

    const float size = static_cast<float>(tile.size);
    float left = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    float top = std::numeric_limits<float>::max();
    float bottom = std::numeric_limits<float>::lowest();

    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 offset{(corner & 1) ? bounds.radius : -bounds.radius, (corner & 2) ? bounds.radius : -bounds.radius,
                          (corner & 4) ? bounds.radius : -bounds.radius};
        const Vec4 clip = tile.matrix * Vec4{bounds.centre.x + offset.x, bounds.centre.y + offset.y, bounds.centre.z + offset.z, 1.0f};
        const float column = (clip.x / clip.w + 1.0f) * 0.5f * size;
        const float row = (1.0f - clip.y / clip.w) * 0.5f * size;
        left = std::min(left, column);
        right = std::max(right, column);
        top = std::min(top, row);
        bottom = std::max(bottom, row);
    }

    const float first = std::clamp(std::floor(left) - 1.0f, 0.0f, size);
    const float last = std::clamp(std::ceil(right) + 1.0f, 0.0f, size);
    const float firstRow = std::clamp(std::floor(top) - 1.0f, 0.0f, size);
    const float lastRow = std::clamp(std::ceil(bottom) + 1.0f, 0.0f, size);

    if (last <= first || lastRow <= firstRow) {
        return std::nullopt;
    }

    return ShadowPatch{tile.x + static_cast<std::uint32_t>(first), tile.y + static_cast<std::uint32_t>(firstRow),
                       static_cast<std::uint32_t>(last - first), static_cast<std::uint32_t>(lastRow - firstRow)};
}

/*
* The matrix is compared exactly: a light that moved by any amount sees a different image. Lights standing still are
* given the same matrix every frame, so they match.
*/
bool ShadowTileMemory::isCurrent(const ShadowTile& tile, std::uint64_t signature) const {
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
        const bool sameView = entry.tile.x == tile.x && entry.tile.y == tile.y && entry.tile.size == tile.size && entry.tile.clipPlane.x == tile.clipPlane.x &&
                              entry.tile.clipPlane.y == tile.clipPlane.y && entry.tile.clipPlane.z == tile.clipPlane.z &&
                              entry.tile.clipPlane.w == tile.clipPlane.w && sameMatrix(entry.tile.matrix, tile.matrix);
        return sameView && entry.signature == signature;
    });
}

void ShadowTileMemory::remember(const ShadowTile& tile, std::uint64_t signature) {
    std::erase_if(entries, [&](const Entry& entry) {
        return overlap(entry.tile, tile);
    });

    entries.push_back(Entry{tile, signature});
}

void ShadowTileMemory::forgetAll() {
    entries.clear();
}
