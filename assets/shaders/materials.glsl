// The scene's textures and materials, as the shaders that colour surfaces read them (triangle.frag, ray_debug.frag).
// Resource declarations only (no functions).
#ifndef MATERIALS_GLSL
#define MATERIALS_GLSL

#extension GL_EXT_nonuniform_qualifier : require

// Every texture there is, in one array (bindless, render/vulkan/bindless_textures.h): slot 0 is white, which a missing
// texture multiplies by, and each uploaded texture sits at its handle plus one. Neighbouring pixels may belong to
// different objects, so an index into it is marked nonuniformEXT: without that, the GPU may assume one texture for a
// whole group of pixels shaded together.
layout(set = 0, binding = 0) uniform sampler2D textures[];

// Must match GpuMaterial in render/gpu/material_uniforms.h.
struct MaterialData {
    vec4 baseColour;
    vec4 surfaceParameters; // x: metallic, y: roughness, z: normal map scale (0 without a normal map), w: occlusion strength (0 without an occlusion texture)
    vec4 emissive;          // rgb: light the surface gives off itself, multiplied by the emissive texture
    uvec4 textureSlots;     // in the texture array: x colour, y metallic-roughness (linear), z normal map (linear), w emissive (sRGB)
    uvec4 extra;            // x: baked occlusion (linear, red channel); y: flags (MATERIAL_DOUBLE_SIDED, MATERIAL_CUT_OUT, MATERIAL_BLEND, MATERIAL_REFRACTS); z: transmission (linear, red); w: thickness (linear, green)
    vec4 transmission;      // x: glass's transmission, 0..1; y: thick glass's thickness (model units); z: 1 over its attenuation distance; w: its index of refraction
    vec4 attenuation;       // rgb: the colour light keeps after the attenuation distance
};

layout(std430, set = 1, binding = 2) readonly buffer Materials {
    MaterialData materials[];
};

#endif
