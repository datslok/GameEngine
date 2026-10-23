#pragma once

#include "scene/asset_handles.h"
#include "scene/material.h"

#include <cstdint>

/*
* Textures are read from one big array (bindless, see VulkanRenderer): slot 0 holds the white stand-in, which a missing
* texture multiplies by (a colour or factor times 1), and every uploaded texture sits at its handle plus one, so the
* numbering follows the AssetManager's.
*/
inline constexpr std::uint32_t whiteTextureSlot = 0;

std::uint32_t bindlessTextureSlot(TextureHandle texture);

/*
* Mirrors the MaterialData struct in triangle.frag's material storage buffer byte for byte (std430, all vec4s).
*/
struct GpuMaterial {
    float baseColour[4];     // rgb 0..1; a: a cut-out's opacity (1 when opaque)
    float surface[4];        // x: metallic, y: roughness, both 0..1; z: normal map scale (0 without a normal map); w: occlusion strength, 0..1 (0 without an occlusion texture)
    float emissive[4];       // rgb: light the surface gives off itself, multiplied by the emissive texture; w: a cut-out's alpha cutoff (0 when opaque)
    std::uint32_t textures[4]; // slots in the texture array: x colour, y metallic-roughness, z normal map, w emissive
    std::uint32_t extra[4];    // x: the occlusion texture's slot; y: flags (MATERIAL_DOUBLE_SIDED, MATERIAL_CUT_OUT, render/gpu/material_shared.h); the rest 0
};

static_assert(sizeof(GpuMaterial) == 80, "GpuMaterial must match the shader's MaterialData struct");

// Convert a material into the shader's layout.
GpuMaterial packMaterial(const Material& material);
