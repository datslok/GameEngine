#include "render/gpu/material_uniforms.h"
#include "core/srgb.h"

/*
* Byte colours are sRGB, like the textures they multiply, so they are decoded to linear light for the lighting maths.
* Materials are always opaque for now, so alpha is 1.
*/
MaterialUniformData packMaterial(const Material& material) {
    MaterialUniformData data{};

    data.baseColour[0] = srgbByteToLinear(material.colour.r);
    data.baseColour[1] = srgbByteToLinear(material.colour.g);
    data.baseColour[2] = srgbByteToLinear(material.colour.b);
    data.baseColour[3] = 1.0f;

    data.specular[0] = material.specularStrength;
    data.specular[1] = material.shininess;

    data.emissive[0] = material.emissive.x;
    data.emissive[1] = material.emissive.y;
    data.emissive[2] = material.emissive.z;

    return data;
}
