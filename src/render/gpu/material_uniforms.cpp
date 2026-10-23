#include "render/gpu/material_uniforms.h"
#include "render/gpu/material_shared.h"
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
* care: white times the emissive colour is the colour itself.
*/
GpuMaterial packMaterial(const Material& material) {
    GpuMaterial data{};

    data.baseColour[0] = srgbByteToLinear(material.colour.r);
    data.baseColour[1] = srgbByteToLinear(material.colour.g);
    data.baseColour[2] = srgbByteToLinear(material.colour.b);
    data.baseColour[3] = material.alphaMode != AlphaMode::Opaque ? std::clamp(material.opacity, 0.0f, 1.0f) : 1.0f;

    data.surface[0] = std::clamp(material.metallic, 0.0f, 1.0f);
    data.surface[1] = std::clamp(material.roughness, 0.0f, 1.0f);
    data.surface[2] = material.normalTexture.isValid() ? material.normalScale : 0.0f;
    data.surface[3] = material.occlusionTexture.isValid() ? std::clamp(material.occlusionStrength, 0.0f, 1.0f) : 0.0f;

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
                    (material.alphaMode == AlphaMode::Blend ? MATERIAL_BLEND : 0u);
    data.extra[2] = bindlessTextureSlot(material.transmissionTexture);
    data.transmission[0] = std::clamp(material.transmission, 0.0f, 1.0f);

    return data;
}
