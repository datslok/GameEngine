#pragma once

#include "scene/material.h"

/*
* Mirrors the MaterialData block in triangle.frag byte for byte (std140, all vec4).
*/
struct MaterialUniformData {
    float baseColour[4]; // rgba, 0..1
    float specular[4];   // x: strength, y: shininess
};

static_assert(sizeof(MaterialUniformData) == 32, "MaterialUniformData must match the shader's MaterialData block");

// Convert a material into the shader's layout.
MaterialUniformData packMaterial(const Material& material);
