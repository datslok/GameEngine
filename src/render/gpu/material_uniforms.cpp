#include "render/gpu/material_uniforms.h"
#include "core/srgb.h"

#include <algorithm>

/*
* Byte colours are sRGB, like the textures they multiply, so they are decoded to linear light for the lighting maths.
* Materials are always opaque for now, so alpha is 1. Metallic and roughness are shares, clamped to 0..1 (the shader
* raises roughness to its smallest drawable value after the texture multiplies it). Without a normal map the scale goes as 0,
* which makes the shader ignore the white texture standing in for one (render/gpu/normal_mapping.h).
*/
MaterialUniformData packMaterial(const Material& material) {
    MaterialUniformData data{};

    data.baseColour[0] = srgbByteToLinear(material.colour.r);
    data.baseColour[1] = srgbByteToLinear(material.colour.g);
    data.baseColour[2] = srgbByteToLinear(material.colour.b);
    data.baseColour[3] = 1.0f;

    data.surface[0] = std::clamp(material.metallic, 0.0f, 1.0f);
    data.surface[1] = std::clamp(material.roughness, 0.0f, 1.0f);
    data.surface[2] = material.normalTexture.isValid() ? material.normalScale : 0.0f;

    data.emissive[0] = material.emissive.x;
    data.emissive[1] = material.emissive.y;
    data.emissive[2] = material.emissive.z;

    return data;
}
