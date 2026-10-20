// The sky as the frame's scene set holds it (render/gpu/atmosphere.h): the flat ambient light (the game's, plus the
// sky's when there is an atmosphere), the sky view table and the atmosphere, at set 1 bindings 17 to 19. Included by
// triangle.frag and, through scene_probes.glsl, ddgi_trace.comp.
#ifndef SCENE_SKY_GLSL
#define SCENE_SKY_GLSL

#include "atmosphere_tables.glsl"

layout(std430, set = 1, binding = 17) readonly buffer SkyLight {
    vec4 skyLightAmbient;
};

layout(set = 1, binding = 18) uniform sampler2D skyViewTable;

layout(std430, set = 1, binding = 19) readonly buffer AtmosphereData {
    Atmosphere frameAtmosphere;
};

// The light that reaches every surface equally where no probes say otherwise.
vec3 flatAmbient() {
    return skyLightAmbient.rgb;
}

// The sky's light along a world direction (none without an atmosphere): what a probe ray that escapes brings back.
vec3 skyLightTowards(vec3 direction) {
    return frameAtmosphere.ground.w > 0.0 ? skyFromTable(skyViewTable, frameAtmosphere, direction) : vec3(0.0);
}

#endif
