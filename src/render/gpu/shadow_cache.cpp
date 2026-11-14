#include "render/gpu/shadow_cache.h"
#include "core/hash.h"

#include <algorithm>
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

// Only meshes, shapes and the count, never matrices; the glass exactly, so a pane moving or changing redraws at once.
std::uint64_t shadowCasterLayout(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters, const std::vector<std::size_t>& glass) {
    Fnv1aHash hash;
    hash.add(static_cast<std::uint64_t>(casters.size()));

    for (std::size_t index : casters) {
        hash.add(draws[index].mesh.index);
        hash.add(draws[index].shape);
    }

    hash.add(glass.empty() ? std::uint64_t{0} : tintedShadowSignature(draws, {}, glass));
    return hash.value;
}

/*
* The matrix is compared exactly: a light that moved by any amount sees a different image. Lights standing still are
* given the same matrix every frame, so they match.
*/
bool ShadowTileMemory::isCurrent(const ShadowTile& tile, std::uint64_t signature) const {
    return isCurrent(tile, signature, 0, 0.0);
}

bool ShadowTileMemory::isCurrent(const ShadowTile& tile, std::uint64_t signature, std::uint64_t layout, double now) const {
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
        const bool sameView = entry.tile.x == tile.x && entry.tile.y == tile.y && entry.tile.size == tile.size && entry.tile.clipPlane.x == tile.clipPlane.x &&
                              entry.tile.clipPlane.y == tile.clipPlane.y && entry.tile.clipPlane.z == tile.clipPlane.z &&
                              entry.tile.clipPlane.w == tile.clipPlane.w && sameMatrix(entry.tile.matrix, tile.matrix);
        const bool onlyMoved = tile.refreshSeconds > 0.0f && entry.layout == layout && now - entry.drawnAt < static_cast<double>(tile.refreshSeconds);
        return sameView && (entry.signature == signature || onlyMoved);
    });
}

void ShadowTileMemory::remember(const ShadowTile& tile, std::uint64_t signature, std::uint64_t layout, double now) {
    std::erase_if(entries, [&](const Entry& entry) {
        return overlap(entry.tile, tile);
    });

    entries.push_back(Entry{tile, signature, layout, now});
}

void ShadowTileMemory::forgetAll() {
    entries.clear();
}
