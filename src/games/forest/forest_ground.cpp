#include "games/forest/forest_ground.h"

#include "assets/image_loader.h"
#include "games/forest/hanging_moss.h"
#include "games/forest/leaf_clusters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace {
    // A height map only needs a depth to count for the blend (render/gpu/ground_layers.h); this is also its parallax
    // depth were the layer used alone, as a share of its texture's width.
    constexpr float layerHeightScale = 0.02f;

    // The stand-in leaf's texels a cell, and the clusters' seed.
    constexpr int standInLeafTexels = 128;
    constexpr std::uint32_t forestClusterSeed = 0x434c5553u;

    std::string findRole(const std::filesystem::path& folder, const char* role) {
        for (const char* extension : {".png", ".jpg"}) {
            const std::filesystem::path file = folder / (std::string{role} + extension);
            std::error_code error;

            if (std::filesystem::is_regular_file(file, error)) {
                return file.string();
            }
        }

        return {};
    }
}

ForestTextureFiles findForestTextureFiles(const std::string& folder) {
    const std::filesystem::path path{folder};
    return ForestTextureFiles{
        .colour = findRole(path, "colour"),
        .normal = findRole(path, "normal"),
        .height = findRole(path, "height"),
        .packedSurface = findRole(path, "arm"),
        .roughness = findRole(path, "roughness"),
        .occlusion = findRole(path, "occlusion"),
        .opacity = findRole(path, "opacity")
    };
}

/*
* The packed map serves as both the metallic-roughness texture (green and blue) and the occlusion texture (red), as
* glTF's occlusion-roughness-metal packing does. A separate roughness map is grey, so its green is the roughness and its
* blue, which would be metal, is cancelled by a metallic factor of 0. The textures are read with the engine's default
* flip, matching the terrain's bottom-left texture coordinates.
*/
float forestBlendDepth(float blendDepth, float texelMetres) {
    const float coarser = std::max(texelMetres / forestFinestPaintTexel, 1.0f);
    return std::min(blendDepth * std::sqrt(coarser), 1.0f);
}

Material forestLayerMaterial(AssetManager& assets, const std::string& assetFolder, const ForestGroundLayer& layer) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / layer.folder).string());
    Material material;

    if (files.colour.empty()) {
        material.colour = layer.standIn;
        material.roughness = layer.standInRoughness;
        return material;
    }

    material.texture = assets.loadTexture(files.colour);
    material.colour = layer.tint;
    material.roughness = 1.0f;

    if (!files.normal.empty()) {
        material.normalTexture = assets.loadTexture(files.normal, true, TextureEncoding::Linear);
    }

    if (!files.height.empty()) {
        material.heightTexture = assets.loadTexture(files.height, true, TextureEncoding::Linear);
        material.heightScale = layerHeightScale;
    }

    if (!files.packedSurface.empty()) {
        const TextureHandle packed = assets.loadTexture(files.packedSurface, true, TextureEncoding::Linear);
        material.metallicRoughnessTexture = packed;
        material.occlusionTexture = packed;
        material.metallic = 1.0f;
    } else {
        if (!files.roughness.empty()) {
            material.metallicRoughnessTexture = assets.loadTexture(files.roughness, true, TextureEncoding::Linear);
        }

        if (!files.occlusion.empty()) {
            material.occlusionTexture = assets.loadTexture(files.occlusion, true, TextureEncoding::Linear);
        }

        material.metallic = 0.0f;
    }

    return material;
}

namespace {
    // A leaf atlas's colour with its opacity map's red (a grey image's every channel) in the alpha.
    ImageData colourWithOpacity(const ForestTextureFiles& files) {
        ImageData colour = loadImage(files.colour);
        const ImageData opacity = loadImage(files.opacity);

        if (opacity.width != colour.width || opacity.height != colour.height) {
            throw std::runtime_error("A leaf atlas's opacity map is not the size of its colour map: " + files.opacity);
        }

        for (std::size_t texel = 0; texel < colour.pixels.size() / 4; ++texel) {
            colour.pixels[texel * 4 + 3] = opacity.pixels[texel * 4];
        }

        return colour;
    }

    // A double-sided cut-out at the leaves' cutoff, as every leaf material is.
    Material leafCutOut() {
        Material material;
        material.alphaMode = AlphaMode::Mask;
        material.alphaCutoff = forestLeafCutoff;
        material.doubleSided = true;
        return material;
    }
}

Material forestLeafMaterial(AssetManager& assets, const std::string& assetFolder, const char* leafFolder, Pixel tint) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / leafFolder).string());
    Material material = leafCutOut();

    if (files.colour.empty() || files.opacity.empty()) {
        material.colour = forestLeafStandIn;
        material.roughness = 0.6f;
        return material;
    }

    material.texture = assets.addTexture(colourWithOpacity(files), forestLeafCutoff);
    material.colour = tint;
    material.roughness = 1.0f;
    material.metallic = 0.0f;

    if (!files.normal.empty()) {
        material.normalTexture = assets.loadTexture(files.normal, true, TextureEncoding::Linear);
    }

    if (!files.roughness.empty()) {
        material.metallicRoughnessTexture = assets.loadTexture(files.roughness, true, TextureEncoding::Linear);
    }

    return material;
}

/*
* The scan's normal and roughness maps must match its colour's size, as the clusters sample them at the same places; a
* scan missing either, or the whole folder, falls back to the stand-in leaf, so the trees always have clusters.
*/
Material forestLeafClusterMaterial(AssetManager& assets, const std::string& assetFolder, Pixel tint, int cellTexels, float leafShare) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / forestTreeLeafFolder).string());
    LeafAtlas source;

    if (!files.colour.empty() && !files.opacity.empty() && !files.normal.empty() && !files.roughness.empty()) {
        source = LeafAtlas{colourWithOpacity(files), loadImage(files.normal), loadImage(files.roughness), forestTreeLeafColumns, forestTreeLeafRows};
        source.normal.encoding = TextureEncoding::Linear;
        source.roughness.encoding = TextureEncoding::Linear;

        if (source.normal.width != source.colour.width || source.roughness.width != source.colour.width ||
            source.normal.height != source.colour.height || source.roughness.height != source.colour.height) {
            throw std::runtime_error("A leaf atlas's normal or roughness map is not the size of its colour map: " + files.colour);
        }
    } else {
        source = standInLeafAtlas(standInLeafTexels, forestTreeLeafColumns, forestTreeLeafRows, forestLeafStandIn);
    }

    LeafAtlas clusters = composeLeafClusters(source, cellTexels, leafShare, forestClusterSeed);
    Material material = leafCutOut();
    material.texture = assets.addTexture(std::move(clusters.colour), forestLeafCutoff);
    material.normalTexture = assets.addTexture(std::move(clusters.normal));
    material.metallicRoughnessTexture = assets.addTexture(std::move(clusters.roughness));
    material.colour = tint;
    material.roughness = 1.0f;
    material.metallic = 0.0f;
    return material;
}

/*
* The mean is taken in sRGB bytes, as the colour is given: close enough for a tint, which the strands then shade
* lighter and darker.
*/
Material forestMossMaterial(AssetManager& assets, const std::string& assetFolder) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / forestMossFolder).string());
    Pixel colour = forestMossStandIn;

    if (!files.colour.empty()) {
        const ImageData scan = loadImage(files.colour);
        std::array<double, 3> sum{0.0, 0.0, 0.0};

        for (std::size_t texel = 0; texel < scan.pixels.size() / 4; ++texel) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                sum[channel] += scan.pixels[texel * 4 + channel];
            }
        }

        const double texels = static_cast<double>(std::max<std::size_t>(scan.pixels.size() / 4, 1));
        colour = Pixel{static_cast<std::uint8_t>(sum[0] / texels + 0.5), static_cast<std::uint8_t>(sum[1] / texels + 0.5),
                       static_cast<std::uint8_t>(sum[2] / texels + 0.5)};
    }

    Material material;
    material.alphaMode = AlphaMode::Mask;
    material.alphaCutoff = forestLeafCutoff;
    material.doubleSided = true;
    material.roughness = 0.9f;
    material.metallic = 0.0f;
    material.texture = assets.addTexture(mossStrandImage(forestMossTextureWidth, forestMossTextureHeight, colour, 0x4d4f5353u), forestLeafCutoff);
    return material;
}
