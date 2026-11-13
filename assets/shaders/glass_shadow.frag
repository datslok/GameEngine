#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The coloured shadow map's glass (render/gpu/transparency.h), drawn into a light's tile at half the shadow atlas's size:
* each pane's share of the light (glassFaceTransmittance: what it lets through at its angle to the light, times what its
* thickness absorbs; 1 for a face turned away from the light, as the rays count them) multiplied into the tint, and its
* depth into the nearest glass depth (blended: the tint multiplied, the larger reversed-Z depth kept). A pane behind
* something solid seen from the light (the atlas's depth at this texel, just drawn) is left out.
*/
#include "render/gpu/shader_shared.h"
#include "render/gpu/material_shared.h"
#include "frame_data.glsl"
#include "materials.glsl"
#include "scene_shadow_tiles.glsl"
#include "transparency.glsl"

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 towardsLight;
layout(location = 4) flat in uint materialIndex;
layout(location = 5) flat in float objectScale;

layout(location = 0) out vec4 tint;
layout(location = 1) out vec4 glassDepth;

void main() {
    // The atlas's texel under this one's top-left corner: its depth was drawn before this pass.
    float solidDepth = texelFetch(shadowDepths, ivec2(gl_FragCoord.xy) * GLASS_SHADOW_MAP_DIVISOR, 0).r;

    if (glassHiddenFromLight(gl_FragCoord.z, solidDepth)) {
        discard;
    }

    MaterialData material = materials[materialIndex];
    vec4 albedo = texture(textures[nonuniformEXT(material.textureSlots.x)], textureUv) * material.baseColour;
    float metallic = clamp(material.surfaceParameters.x * texture(textures[nonuniformEXT(material.textureSlots.y)], textureUv).b, 0.0, 1.0);
    float transmission = material.transmission.x * texture(textures[nonuniformEXT(material.extra.z)], textureUv).r;
    float coverage = (material.extra.y & MATERIAL_BLEND) != 0u ? albedo.a : 1.0;
    float thickness = material.transmission.y * texture(textures[nonuniformEXT(material.extra.w)], textureUv).g * objectScale;
    vec3 absorbed = beerAttenuation(material.attenuation.rgb, material.transmission.z, thickness);

    float normalLength = length(worldNormal);
    vec3 toLight = normalize(towardsLight);
    float facing = normalLength > 0.0 ? dot(worldNormal, toLight) / normalLength : 1.0;
    bool doubleSided = (material.extra.y & MATERIAL_DOUBLE_SIDED) != 0u;

    tint = vec4(glassFaceTransmittance(facing < 0.0, doubleSided, coverage, transmission, metallic, albedo.rgb, facing, absorbed), 1.0);
    glassDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);
}
