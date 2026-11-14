#include "render/gpu/material_uniforms.h"
#include "render/gpu/ground_layers.h"
#include "render/gpu/material_shared.h"
#include "render/gpu/transparency.h"
#include "core/srgb.h"

#include <algorithm>

// No branch for "no texture" in the shader: it reads the white stand-in in slot 0 like any other texture.
std::uint32_t bindlessTextureSlot(TextureHandle texture) {
    return texture.isValid() ? texture.index + 1 : whiteTextureSlot;
}

/*
* Byte colours are sRGB, like the textures they multiply, so they are decoded to linear light for the lighting maths.
* Alpha is a cut-out's or blended material's opacity, a cut-out's cutoff in emissive.w (an opaque material packs 1
* and 0, keeping every texel).
* Metallic and roughness are shares, clamped to 0..1 (the shader raises roughness to its smallest drawable value after
* the texture multiplies it). Without a normal map the scale goes as 0, which makes the shader ignore the white texture
* standing in for one (render/gpu/normal_mapping.h). The occlusion strength likewise goes as 0 without an occlusion
* texture, which tells the shader to leave the light from all around as it was. A missing emissive texture needs no such
* care: white times the emissive colour is the colour itself. An infinite attenuation distance packs as 0 (1 / infinity),
* absorbing nothing. A clear coat's normal map scale likewise goes as 0 without its texture, and a transparent
* material's coat as none: glass is a varnish-like surface already. A height map's depth goes as 0 without its texture,
* and on cut-outs (whose holes the depth prepass cuts at the unshifted place) and transparent materials.
*/
GpuMaterial packMaterial(const Material& material) {
    GpuMaterial data{};

    data.baseColour[0] = srgbByteToLinear(material.colour.r);
    data.baseColour[1] = srgbByteToLinear(material.colour.g);
    data.baseColour[2] = srgbByteToLinear(material.colour.b);
    data.baseColour[3] = material.alphaMode != AlphaMode::Opaque ? std::clamp(material.opacity, 0.0f, 1.0f) : 1.0f;

    data.surface[0] = std::clamp(material.metallic, 0.0f, 1.0f);
    data.surface[1] = std::clamp(material.roughness, 0.0f, 1.0f);
    data.surface[2] = material.normalTexture.isValid() || !material.groundLayers.empty() ? material.normalScale : 0.0f;
    data.surface[3] = material.occlusionTexture.isValid() || !material.groundLayers.empty() ? std::clamp(material.occlusionStrength, 0.0f, 1.0f) : 0.0f;

    data.emissive[0] = material.emissive.x;
    data.emissive[1] = material.emissive.y;
    data.emissive[2] = material.emissive.z;
    data.emissive[3] = isCutOut(material) ? std::max(material.alphaCutoff, 0.0f) : 0.0f;

    data.textures[0] = bindlessTextureSlot(material.texture);
    data.textures[1] = bindlessTextureSlot(material.metallicRoughnessTexture);
    data.textures[2] = bindlessTextureSlot(material.normalTexture);
    data.textures[3] = bindlessTextureSlot(material.emissiveTexture);
    data.extra[0] = bindlessTextureSlot(material.occlusionTexture);
    data.extra[1] = (material.doubleSided ? MATERIAL_DOUBLE_SIDED : 0u) | (isCutOut(material) ? MATERIAL_CUT_OUT : 0u) |
                    (material.alphaMode == AlphaMode::Blend ? MATERIAL_BLEND : 0u) | (isRefractive(material) ? MATERIAL_REFRACTS : 0u);
    data.extra[2] = bindlessTextureSlot(material.transmissionTexture);
    data.extra[3] = bindlessTextureSlot(material.thicknessTexture);
    data.transmission[0] = std::clamp(material.transmission, 0.0f, 1.0f);
    data.transmission[1] = std::max(material.thickness, 0.0f);
    data.transmission[2] = material.attenuationDistance > 0.0f ? 1.0f / material.attenuationDistance : 0.0f;
    data.transmission[3] = material.ior;
    data.attenuation[0] = material.attenuationColour.x;
    data.attenuation[1] = material.attenuationColour.y;
    data.attenuation[2] = material.attenuationColour.z;

    if (!isTransparent(material)) {
        data.coatAndHeight[0] = std::clamp(material.clearCoat, 0.0f, 1.0f);
        data.coatAndHeight[1] = std::clamp(material.clearCoatRoughness, 0.0f, 1.0f);
        data.coatAndHeight[2] = material.clearCoatNormalTexture.isValid() ? material.clearCoatNormalScale : 0.0f;
    }

    if (!isTransparent(material) && !isCutOut(material) && material.heightTexture.isValid()) {
        data.coatAndHeight[3] = std::max(material.heightScale, 0.0f);
    }

    data.coatAndHeightTextures[0] = bindlessTextureSlot(material.clearCoatTexture);
    data.coatAndHeightTextures[1] = bindlessTextureSlot(material.clearCoatRoughnessTexture);
    data.coatAndHeightTextures[2] = bindlessTextureSlot(material.clearCoatNormalTexture);
    data.coatAndHeightTextures[3] = bindlessTextureSlot(material.heightTexture);

    // A blended ground's repeats, weights and blend depth; its layers' places are the material table's to fill.
    if (!material.groundLayers.empty()) {
        for (std::size_t layer = 0; layer < 4; ++layer) {
            data.groundRepeats[layer] = material.groundRepeats[layer];
        }

        data.groundWeights[0] = bindlessTextureSlot(material.groundWeights);
        data.groundWeights[1] = (material.groundProjected ? groundProjectedFlag : 0u) | (material.groundPaint == GroundPaint::Strata ? groundStrataFlag : 0u) |
                                (material.groundHexTiling ? groundHexFlag : 0u);
        data.groundBlend[0] = std::max(material.groundBlendDepth, 0.0f);

        // Strata: where the strip starts and how many of it a metre is; the layer that takes over facing up.
        if (material.groundPaint == GroundPaint::Strata) {
            const float span = material.strataTop - material.strataBottom;
            data.groundWeights[2] = material.groundTopLayer >= 0 ? static_cast<std::uint32_t>(material.groundTopLayer) + 1 : 0u;
            data.groundBlend[1] = material.strataBottom;
            data.groundBlend[2] = span != 0.0f ? 1.0f / span : 0.0f;
            data.groundFacing[0] = material.groundTopStart;
            data.groundFacing[1] = material.groundTopFull;

            // Wavy beds, only with their texture.
            if (material.strataWarp.isValid() && material.strataWarpTile > 0.0f) {
                data.groundSlots[0] = bindlessTextureSlot(material.strataWarp);
                data.groundBlend[3] = std::max(material.strataWarpMetres, 0.0f);
                data.groundFacing[2] = 1.0f / material.strataWarpTile;
            }
        }

        // Streaks down the sides of projected layers, only with their texture.
        if (material.groundProjected && material.groundStreaks.isValid() && material.streakWidth > 0.0f && material.streakLength > 0.0f) {
            data.groundSlots[1] = bindlessTextureSlot(material.groundStreaks);
            data.groundStreaks[0] = std::clamp(material.streakStrength, 0.0f, 1.0f);
            data.groundStreaks[1] = 1.0f / material.streakWidth;
            data.groundStreaks[2] = 1.0f / material.streakLength;
        }

        // Tone variation over projected layers, only with its texture.
        if (material.groundProjected && material.groundVariation.isValid() && material.variationTile > 0.0f) {
            data.groundSlots[2] = bindlessTextureSlot(material.groundVariation);
            data.groundStreaks[3] = std::clamp(material.variationStrength, 0.0f, 1.0f);
            data.groundFacing[3] = 1.0f / material.variationTile;
        }
    }

    return data;
}
