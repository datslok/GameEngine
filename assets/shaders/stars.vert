#version 460
#extension GL_GOOGLE_include_directive : require

/*
* The stars (render/gpu/stars.h), one instance each: a square a pixel either side of where the star falls, at the far
* depth (drawn with the depth test equal, so only where nothing else was drawn, like the sky), added over the sky. The
* star is turned from the equatorial frame into the world, its light dimmed and reddened by the air on the way down
* (none below the horizon) and divided by its pixel's solid angle into the radiance its splat shares out. Stars behind
* the camera, below the horizon or behind the moon's disc are moved off screen.
*/
#include "atmosphere_tables.glsl"
#include "render/gpu/depth_shared.h"
#include "stars.glsl"

// Must match GpuStar in render/gpu/stars.h.
struct Star {
    vec4 directionAndMagnitude;
    vec4 colour;
};

layout(std430, set = 0, binding = 0) readonly buffer AtmosphereData {
    Atmosphere atmosphere;
} data;

layout(set = 0, binding = 1) uniform sampler2D transmittanceTable;

layout(std430, set = 0, binding = 2) readonly buffer Stars {
    Star stars[];
};

// GpuStarView: the camera's rays as the sky pass takes them, then the rotation's rows, brightness and contrast in w.
layout(push_constant) uniform StarView {
    vec4 right;
    vec4 up;
    vec4 forward;
    vec4 originAndView;
    vec4 screen;
    vec4 toWorld[3];
} view;

layout(location = 0) flat out vec3 radiance;
layout(location = 1) flat out vec2 starPixel;

const float STAR_PI = 3.14159265358979;

void hide() {
    gl_Position = vec4(2.0, 2.0, GPU_FAR_DEPTH, 1.0);
    radiance = vec3(0.0);
    starPixel = vec2(0.0);
}

void main() {
    // Two triangles: the square's corners, a pixel either side of the star.
    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    Star star = stars[gl_InstanceIndex];
    vec3 equatorial = star.directionAndMagnitude.xyz;
    vec3 direction = vec3(dot(view.toWorld[0].xyz, equatorial), dot(view.toWorld[1].xyz, equatorial), dot(view.toWorld[2].xyz, equatorial));
    float along = dot(direction, view.forward.xyz);
    Atmosphere atmosphere = data.atmosphere;
    bool behindMoon = atmosphere.moon.w < 1.0 && dot(direction, atmosphere.moon.xyz) >= atmosphere.moon.w;

    if (along <= 0.0 || behindMoon) {
        hide();
        return;
    }

    vec3 throughAir = transmittanceFromTable(transmittanceTable, atmosphere, atmosphere.radii.w, direction.y);

    if (all(equal(throughAir, vec3(0.0)))) {
        hide();
        return;
    }

    vec2 size = view.screen.xy;
    vec2 onScreen = starScreenPosition(view.right.xyz, view.up.xyz, view.forward.xyz, direction);
    vec3 illuminance = STAR_PI * starLight(star.directionAndMagnitude.w, view.toWorld[0].w, view.toWorld[1].w) * star.colour.rgb * throughAir;

    radiance = illuminance / pixelSolidAngle(view.right.xyz, view.up.xyz, size.x, size.y, along);
    starPixel = vec2((onScreen.x + 1.0) * 0.5 * size.x, (1.0 - onScreen.y) * 0.5 * size.y);
    gl_Position = vec4(onScreen + corners[gl_VertexIndex] * 2.0 / size, GPU_FAR_DEPTH, 1.0);
}
