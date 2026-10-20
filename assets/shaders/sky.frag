#version 460
#extension GL_GOOGLE_include_directive : require

/*
* The sky (render/gpu/atmosphere.h), added over the background wherever nothing was drawn (the full-screen triangle at
* the far depth, depth test equal): each pixel's camera ray, as the ray debug view finds it, looks up the sky view table,
* and inside the sun's disc as drawn adds its radiance through the air, and inside the moon's its radiance shaded by its
* phase.
*/
#include "atmosphere_tables.glsl"
#include "ray_tracing.glsl"

layout(location = 0) out vec4 colour;

layout(std430, set = 0, binding = 0) readonly buffer AtmosphereData {
    Atmosphere atmosphere;
} data;

layout(set = 0, binding = 1) uniform sampler2D skyViewTable;

// The camera's rays (render/gpu/ray_tracing.h, GpuRayDebugView; the view number unused).
layout(push_constant) uniform CameraRays {
    vec4 right;
    vec4 up;
    vec4 forward;
    vec4 originAndView;
    vec4 screen;
} camera;

void main() {
    vec2 onScreen = gl_FragCoord.xy / camera.screen.xy;
    vec3 direction = normalize(cameraRayDirection(camera.right.xyz, camera.up.xyz, camera.forward.xyz, onScreen.x * 2.0 - 1.0, 1.0 - onScreen.y * 2.0));
    vec3 sky = skyFromTable(skyViewTable, data.atmosphere, direction);

    if (dot(direction, data.atmosphere.sun.xyz) >= data.atmosphere.sun.w) {
        sky += data.atmosphere.sunDisc.rgb;
    }

    // The moon's disc, a ball lit by the sun: its phase.
    if (dot(direction, data.atmosphere.moon.xyz) >= data.atmosphere.moon.w) {
        sky += data.atmosphere.moonDisc.rgb * lommelSeeliger(direction, data.atmosphere.moon.xyz, data.atmosphere.moonIlluminance.w, data.atmosphere.sun.xyz);
    }

    colour = vec4(sky, 1.0);
}
