// The wind force field as the scene's shaders read it (render/gpu/wind.h): its record and its springs, this frame's and
// last frame's (for motion vectors), and what they do to a vertex. Included by the passes that sway what they draw.
#ifndef WIND_DATA_GLSL
#define WIND_DATA_GLSL

#include "wind.glsl"

layout(std430, set = 1, binding = 37) readonly buffer WindFieldRecord {
    WindField windField;
};

// Each texel a cell's springs, layer cascade * WIND_CATEGORIES + category: xy displacement, zw velocity. Sampled with
// wrapping and linear filtering, so a place reads its cell's spring at place / (cell * WIND_CELLS) whichever window
// texel holds it (cell c's middle, (c + 0.5) cells along, at texel c's middle).
layout(set = 1, binding = 38) uniform sampler2DArray windSprings;
layout(set = 1, binding = 39) uniform sampler2DArray windPreviousSprings;

/*
* A category's spring at a place on the ground, this frame's or last frame's (in last frame's windows): the near
* window's, passing into the far one's over its last cells (windWindowShare), nothing outside both.
*/
vec2 windSpringAt(vec2 place, int category, bool previous) {
    vec2 spring = vec2(0.0);
    float left = 1.0;

    for (int cascade = 0; cascade < WIND_CASCADES; ++cascade) {
        ivec4 window = windField.windows[cascade];
        float share = windWindowShare(place, previous ? window.zw : window.xy, windField.cells[cascade].x);

        if (share > 0.0) {
            vec3 coordinate = vec3(place * windField.cells[cascade].y, float(cascade * WIND_CATEGORIES + category));
            vec2 value = previous ? textureLod(windPreviousSprings, coordinate, 0.0).xy : textureLod(windSprings, coordinate, 0.0).xy;
            spring += value * (share * left);
        }

        left *= 1.0 - share;
    }

    return spring;
}

/*
* Where the wind moves a vertex of an instance (windSwayOffset), this frame or last frame (previous: its springs and
* time, for the velocity pass). model: the instance's world matrix, sway and scale its record's (GpuInstance::sway,
* uniformScale), position the vertex in the mesh's frame, world where it is before swaying (lifted onto the land when it
* follows it), eye the camera. Trees, plants and hanging things read their spring at their foot, the model's origin,
* so everything sharing their pose bends as one; grass at the ground below each vertex. Nothing while nothing sways.
*/
vec3 windSwayOfVertex(mat4 model, vec4 sway, float scale, vec3 position, vec3 world, vec3 eye, bool previous) {
    if (sway.x < 0.0 || windField.sway.x == 0.0) {
        return vec3(0.0);
    }

    int category = int(sway.x);
    vec3 footInMesh = category == WIND_CATEGORY_GRASS ? vec3(position.x, 0.0, position.z) : vec3(0.0);
    vec3 fromFoot = mat3(model) * (position - footInMesh);
    vec3 foot = world - fromFoot;
    vec2 spring = windSpringAt(foot.xz, category, previous);
    float fade = windSwayFade(distance(foot, eye), windField.sway.y, windField.sway.z);
    float seconds = previous ? windField.sway.w : windField.wind.w;
    return windSwayOffset(fromFoot, position.y, world, sway, scale, spring, fade, seconds);
}

#endif
