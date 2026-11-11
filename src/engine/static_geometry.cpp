#include "engine/static_geometry.h"

#include "core/hash.h"
#include "core/srgb.h"
#include "scene/collision_mesh.h"

#include <algorithm>

namespace {
    // Only a triangle covering more cells than this (a floor of over 300 by 300 m at 1 m cells) goes on the list every
    // ray tests; anything smaller is listed in each cell it touches.
    constexpr int largeTriangleCells = 100000;

    // About how many texels a texture's mean reads.
    constexpr std::size_t meanSamples = 65536;
}

bool blocksSky(const DrawItem& draw) {
    return draw.isStatic && draw.castsShadows && !isTransparent(draw.material);
}

std::uint64_t skyBlockersFingerprint(std::span<const DrawItem> draws) {
    Fnv1aHash hash;

    for (const DrawItem& draw : draws) {
        if (!blocksSky(draw)) {
            continue;
        }

        hash.add(draw.mesh.index);
        hash.add(draw.material.colour.r);
        hash.add(draw.material.colour.g);
        hash.add(draw.material.colour.b);
        hash.add(draw.material.texture.index);
        hash.add(draw.material.metallic);

        for (const auto& row : draw.model.values) {
            for (const float value : row) {
                hash.add(value);
            }
        }
    }

    return hash.value;
}

CollisionSettings skyRayCollision() {
    CollisionSettings settings;
    settings.gridCellSize = 1.0f;
    settings.largeItemCells = largeTriangleCells;
    return settings;
}

/*
* Each blocking draw's mesh moved into the world by its matrix, the same triangles as drawn (a mesh's collision copy,
* scene/collision_mesh). A rebuild only when the fingerprint changes: static draws rarely do (at load, or when a game
* builds or removes a piece).
*/
void StaticGeometry::update(std::span<const DrawItem> draws, const AssetManager& assets) {
    const std::uint64_t now = skyBlockersFingerprint(draws);

    if (version != 0 && now == fingerprint) {
        return;
    }

    fingerprint = now;
    ++version;
    auto built = std::make_shared<StaticLevel>(skyRayCollision());
    auto colours = std::make_shared<std::vector<Vec3>>();
    bool any = false;

    for (const DrawItem& draw : draws) {
        if (!blocksSky(draw)) {
            continue;
        }

        built->add(collisionMesh(assets.getMesh(draw.mesh), draw.model));
        const Material& material = draw.material;
        const Vec3 colour{srgbByteToLinear(material.colour.r), srgbByteToLinear(material.colour.g), srgbByteToLinear(material.colour.b)};
        const Vec3 texel = material.texture.isValid() ? textureMean(material.texture, assets) : Vec3{1.0f, 1.0f, 1.0f};
        const float diffuse = 1.0f - std::clamp(material.metallic, 0.0f, 1.0f);
        colours->resize(built->getTriangleCount(), Vec3{colour.x * texel.x * diffuse, colour.y * texel.y * diffuse, colour.z * texel.z * diffuse});
        any = true;
    }

    level = any ? std::shared_ptr<const StaticLevel>{std::move(built)} : nullptr;
    albedo = any ? std::shared_ptr<const std::vector<Vec3>>{std::move(colours)} : nullptr;
}

/*
* The mean of the decoded texels, not the decoded mean of the bytes: light adds in linear space. A large texture is
* sampled every few texels (at most about 65,000 read), plenty for a mean.
*/
Vec3 StaticGeometry::textureMean(TextureHandle texture, const AssetManager& assets) {
    const auto found = textureMeans.find(texture.index);

    if (found != textureMeans.end()) {
        return found->second;
    }

    const ImageData& image = assets.getTexture(texture);
    const std::size_t texels = static_cast<std::size_t>(image.width) * image.height;
    const std::size_t step = std::max<std::size_t>(1, texels / meanSamples);
    const bool srgb = image.encoding == TextureEncoding::Srgb;
    double sum[3] = {0.0, 0.0, 0.0};
    std::size_t counted = 0;

    for (std::size_t texel = 0; texel < texels && 4 * texel + 2 < image.pixels.size(); texel += step) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const std::uint8_t byte = image.pixels[4 * texel + channel];
            sum[channel] += srgb ? srgbByteToLinear(byte) : static_cast<float>(byte) / 255.0f;
        }

        ++counted;
    }

    const double share = counted > 0 ? 1.0 / static_cast<double>(counted) : 0.0;
    const Vec3 mean = counted > 0 ? Vec3{static_cast<float>(sum[0] * share), static_cast<float>(sum[1] * share), static_cast<float>(sum[2] * share)}
                                  : Vec3{1.0f, 1.0f, 1.0f};
    textureMeans.emplace(texture.index, mean);
    return mean;
}

std::shared_ptr<const StaticLevel> StaticGeometry::getLevel() const {
    return level;
}

std::shared_ptr<const std::vector<Vec3>> StaticGeometry::getAlbedo() const {
    return albedo;
}

std::uint64_t StaticGeometry::getVersion() const {
    return version;
}
