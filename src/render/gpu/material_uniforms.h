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
    float baseColour[4];     // rgb 0..1; a: a cut-out's or blended material's opacity (1 when opaque)
    float surface[4];        // x: metallic, y: roughness, both 0..1; z: normal map scale (0 without a normal map); w: occlusion strength, 0..1 (0 without an occlusion texture)
    float emissive[4];       // rgb: light the surface gives off itself, multiplied by the emissive texture; w: a cut-out's alpha cutoff (0 when opaque)
    std::uint32_t textures[4]; // slots in the texture array: x colour, y metallic-roughness, z normal map, w emissive
    // x: the occlusion texture's slot; y: flags (MATERIAL_DOUBLE_SIDED, MATERIAL_CUT_OUT, render/gpu/material_shared.h);
    // z: the transmission texture's slot; w: the thickness texture's slot
    std::uint32_t extra[4];
    // x: glass's transmission, 0..1 (render/gpu/transparency.h); y: thick glass's thickness (model units, 0 for thin
    // glass); z: 1 over its attenuation distance (0 absorbs nothing); w: its index of refraction
    float transmission[4];
    float attenuation[4];    // rgb: the colour light keeps after the attenuation distance; w: 0
    // A clear coat (render/gpu/brdf.h, addClearCoat): x how much, 0..1 (0 none, and always on transparent materials);
    // y its roughness, 0..1; z its normal map's scale (0 without one, so it lies on the geometry); and w the height map's
    // depth for parallax (render/gpu/parallax.h; texture units, 0 for none, and always on cut-outs and transparent materials)
    float coatAndHeight[4];
    std::uint32_t coatAndHeightTextures[4]; // slots in the texture array: x coat amount, y coat roughness, z coat normal map, w height map
    // A blended ground (render/gpu/ground_layers.h; MaterialTable::add fills the layers): each layer's place in the frame's
    // material table plus one (0 none), each layer's repeats over the texture coordinates (a metre when projected); the
    // weights texture's slot (0 for a material without layers), the switches (GROUND_PROJECTED, GROUND_STRATA,
    // render/gpu/ground_layers_shared.h) and the top layer's number plus one (0 none); the blend depth, the strata's
    // bottom, one over their span and the warp's metres; the height of the normal's upward part where the top layer
    // starts and is full, and one over the warp's tile; the warp's and the streaks' texture slots; and the streaks'
    // strength, one over their width and one over their length
    std::uint32_t groundLayers[4];
    float groundRepeats[4];
    std::uint32_t groundWeights[4];
    float groundBlend[4];
    float groundFacing[4];
    std::uint32_t groundSlots[4];
    float groundStreaks[4];
};

static_assert(sizeof(GpuMaterial) == 256, "GpuMaterial must match the shader's MaterialData struct");

// Convert a material into the shader's layout.
GpuMaterial packMaterial(const Material& material);
