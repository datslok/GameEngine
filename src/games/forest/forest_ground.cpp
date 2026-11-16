#include "games/forest/forest_ground.h"

#include "assets/image_loader.h"
#include "games/forest/climbers.h"
#include "games/forest/epiphytes.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/grass.h"
#include "games/forest/hanging_moss.h"
#include "games/forest/leaf_clusters.h"
#include "games/forest/palms.h"
#include "core/height_map.h"

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

Pixel meanColour(const ImageData& image, Pixel fallback) {
    std::array<double, 3> sum{0.0, 0.0, 0.0};
    double weight = 0.0;

    for (std::size_t texel = 0; texel < image.pixels.size() / 4; ++texel) {
        const double share = image.pixels[texel * 4 + 3] / 255.0;
        weight += share;

        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum[channel] += image.pixels[texel * 4 + channel] * share;
        }
    }

    if (weight <= 0.0) {
        return fallback;
    }

    return Pixel{static_cast<std::uint8_t>(sum[0] / weight + 0.5), static_cast<std::uint8_t>(sum[1] / weight + 0.5),
                 static_cast<std::uint8_t>(sum[2] / weight + 0.5)};
}

Material forestPalmLeafMaterial(AssetManager& assets, const std::string& assetFolder) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / forestTreeLeafFolder).string());
    const Pixel colour = !files.colour.empty() && !files.opacity.empty() ? meanColour(colourWithOpacity(files), forestLeafStandIn) : forestLeafStandIn;
    PalmLeafletImages images = palmLeafletImages(forestPalmLeafletTexels, colour, 0x50414c4du);
    const ImageLevel normal = normalMapFromHeight(images.height, forestPalmLeafletDepth);

    Material material;
    material.doubleSided = true;
    material.colour = forestTreeLeafTint;
    material.roughness = 0.6f;
    material.metallic = 0.0f;
    material.texture = assets.addTexture(std::move(images.colour));
    material.normalTexture = assets.addTexture(ImageData{normal.width, normal.height, normal.pixels, TextureEncoding::Linear});
    return material;
}

Material forestAroidLeafMaterial(AssetManager& assets, const std::string& assetFolder) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / forestTreeLeafFolder).string());
    const Pixel colour = !files.colour.empty() && !files.opacity.empty() ? meanColour(colourWithOpacity(files), forestLeafStandIn) : forestLeafStandIn;
    AroidLeafImages images = aroidLeafImages(forestAroidLeafTexels, colour, 0x41524f49u);
    const ImageLevel normal = normalMapFromHeight(images.height, forestAroidLeafDepth);

    Material material;
    material.doubleSided = true;
    material.colour = forestTreeLeafTint;
    material.roughness = forestAroidLeafRoughness;
    material.metallic = 0.0f;
    material.texture = assets.addTexture(std::move(images.colour));
    material.normalTexture = assets.addTexture(ImageData{normal.width, normal.height, normal.pixels, TextureEncoding::Linear});
    return material;
}

Material forestGrassMaterial(AssetManager& assets, const std::string& assetFolder) {
    const ForestGroundLayer& grass = forestGroundLayers[forestGrassLayer];
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / grass.folder).string());
    const Pixel colour = files.colour.empty() ? grass.standIn : meanColour(loadImage(files.colour), grass.standIn);

    Material material;
    material.doubleSided = true;
    material.backKeepsNormal = true;
    material.roughness = forestGrassRoughness;
    material.metallic = 0.0f;
    material.texture = assets.addTexture(grassBladeImage(forestGrassBladeTexels, colour));
    return material;
}

Material forestEpiphyteLeafMaterial(AssetManager& assets, const std::string& assetFolder) {
    const ForestTextureFiles files = findForestTextureFiles((std::filesystem::path{assetFolder} / forestTreeLeafFolder).string());
    const Pixel colour = !files.colour.empty() && !files.opacity.empty() ? meanColour(colourWithOpacity(files), forestLeafStandIn) : forestLeafStandIn;
    EpiphyteLeafImages images = epiphyteLeafImages(forestEpiphyteLeafTexels, colour, 0x45504950u);
    const ImageLevel normal = normalMapFromHeight(images.height, forestEpiphyteLeafDepth);

    Material material;
    material.doubleSided = true;
    material.colour = forestTreeLeafTint;
    material.roughness = forestEpiphyteLeafRoughness;
    material.metallic = 0.0f;
    material.texture = assets.addTexture(std::move(images.colour));
    material.normalTexture = assets.addTexture(ImageData{normal.width, normal.height, normal.pixels, TextureEncoding::Linear});
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
        colour = meanColour(loadImage(files.colour), forestMossStandIn);
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

void visit(ForestLeafLightSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Share", settings.share, {.min = 0.0f, .max = 4.0f, .sliderMax = 2.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Tint red", settings.tintRed, {.min = 0.0f, .max = 4.0f, .sliderMax = 2.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Tint green", settings.tintGreen, {.min = 0.0f, .max = 4.0f, .sliderMax = 2.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Tint blue", settings.tintBlue, {.min = 0.0f, .max = 4.0f, .sliderMax = 2.0f, .applies = FieldApplies::AtStartup});
}

Material withLeafLight(Material leaf, const ForestLeafLightSettings& settings) {
    leaf.diffuseTransmission = settings.share;
    leaf.diffuseTransmissionTint = Vec3{settings.tintRed, settings.tintGreen, settings.tintBlue};
    return leaf;
}
