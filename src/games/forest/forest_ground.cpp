#include "games/forest/forest_ground.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
    // A height map only needs a depth to count for the blend (render/gpu/ground_layers.h); this is also its parallax
    // depth were the layer used alone, as a share of its texture's width.
    constexpr float layerHeightScale = 0.02f;

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
        .occlusion = findRole(path, "occlusion")
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
