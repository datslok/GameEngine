#pragma once

#include "render/gpu/uniform_limits.h"
#include "scene/material.h"

/*
* Mirrors the MaterialData block in triangle.frag byte for byte (std140, all vec4).
*/
struct MaterialUniformData {
    float baseColour[4]; // rgba, 0..1
    float surface[4];    // x: metallic, y: roughness, both 0..1; z: normal map scale (0 without a normal map)
    float emissive[4];   // rgb: light the surface gives off itself
};

static_assert(sizeof(MaterialUniformData) == 48, "MaterialUniformData must match the shader's MaterialData block");
static_assert(sizeof(MaterialUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// Convert a material into the shader's layout.
MaterialUniformData packMaterial(const Material& material);
