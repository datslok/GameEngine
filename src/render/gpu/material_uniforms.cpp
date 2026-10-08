#include "render/gpu/material_uniforms.h"

/*
* Byte colour channels become the shader's 0..1 range. Materials are always opaque for now, so alpha is 1.
*/
MaterialUniformData packMaterial(const Material& material) {
    MaterialUniformData data{};

    data.baseColour[0] = static_cast<float>(material.colour.r) / 255.0f;
    data.baseColour[1] = static_cast<float>(material.colour.g) / 255.0f;
    data.baseColour[2] = static_cast<float>(material.colour.b) / 255.0f;
    data.baseColour[3] = 1.0f;

    data.specular[0] = material.specularStrength;
    data.specular[1] = material.shininess;

    return data;
}
