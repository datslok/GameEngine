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

// Every hanging mesh's vertices (render/gpu/wind.h, packHangingVertex): x the anchors (first in the low 16 bits),
// y the share along and the arm as half floats; and the anchors they hang from.
layout(std430, set = 1, binding = 40) readonly buffer HangingVertices {
    uvec2 hangingVertices[];
};

layout(std430, set = 1, binding = 41) readonly buffer WindAnchors {
    WindAnchorRecord windAnchors[];
};

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

// Where the wind moves a vertex as its own object sways there (windSwayOffset): see windSwayOfVertex.
vec3 windOwnSway(mat4 model, vec4 sway, float scale, vec3 position, vec3 world, vec3 eye, float seconds, bool previous) {
    int category = int(sway.x);
    vec3 footInMesh = category == WIND_CATEGORY_GRASS ? vec3(position.x, 0.0, position.z) : vec3(0.0);
    vec3 fromFoot = mat3(model) * (position - footInMesh);
    vec3 foot = world - fromFoot;
    vec2 spring = windSpringAt(foot.xz, category, previous);
    float fade = windSwayFade(distance(foot, eye), windField.sway.y, windField.sway.z);
    return windSwayOffset(fromFoot, position.y, world, sway, scale, spring, fade, seconds);
}

// How an anchor of a hanging vertex moves: not at all when fixed, as its own object sways at the vertex (own), else as
// its tree does there (windAnchorOffset), its spring and fade at the tree's foot.
vec3 windAnchorMotion(uint anchor, vec3 own, vec3 eye, float seconds, bool previous) {
    if (anchor == uint(WIND_ANCHOR_FIXED)) {
        return vec3(0.0);
    }

    if (anchor == uint(WIND_ANCHOR_OWN)) {
        return own;
    }

    WindAnchorRecord record = windAnchors[anchor];
    vec2 spring = windSpringAt(record.foot.xz, WIND_CATEGORY_TREES, previous);
    float fade = windSwayFade(distance(record.foot.xyz, eye), windField.sway.y, windField.sway.z);
    return windAnchorOffset(record, spring, fade, seconds);
}

/*
* Where the wind moves a vertex of an instance, this frame or last frame (previous: its springs and time, for the
* velocity pass). model: the instance's world matrix, sway and scale its record's (GpuInstance::sway, uniformScale),
* hanging its hanging record (its mesh's first hanging vertex and its swing), vertex the vertex's number in its mesh,
* position the vertex in the mesh's frame, world where it is before swaying (lifted onto the land when it follows it),
* eye the camera. Trees, plants and hanging things read their spring at their foot, the model's origin, so everything
* sharing their pose bends as one; grass at the ground below each vertex. A hanging vertex instead moves as its anchors
* do, blended by its share along, and swings by its arm on the hanging spring where it is. Nothing while nothing sways.
*/
vec3 windSwayOfVertex(mat4 model, vec4 sway, float scale, uvec4 hanging, int vertex, vec3 position, vec3 world, vec3 eye, bool previous) {
    if (sway.x < 0.0 || windField.sway.x == 0.0) {
        return vec3(0.0);
    }

    float seconds = previous ? windField.sway.w : windField.wind.w;

    if (hanging.x == 0u) {
        return windOwnSway(model, sway, scale, position, world, eye, seconds, previous);
    }

    uvec2 record = hangingVertices[hanging.x - 1u + uint(vertex)];
    uint first = record.x & 0xffffu;
    uint second = record.x >> 16u;
    vec2 alongAndArm = unpackHalf2x16(record.y);
    bool needsOwn = first == uint(WIND_ANCHOR_OWN) || second == uint(WIND_ANCHOR_OWN);
    vec3 own = needsOwn ? windOwnSway(model, sway, scale, position, world, eye, seconds, previous) : vec3(0.0);
    vec3 firstMotion = windAnchorMotion(first, own, eye, seconds, previous);
    vec3 secondMotion = windAnchorMotion(second, own, eye, seconds, previous);
    vec2 spring = windSpringAt(world.xz, WIND_CATEGORY_HANGING, previous);
    float fade = windSwayFade(distance(world, eye), windField.sway.y, windField.sway.z);
    vec3 swing = windSwingOffset(alongAndArm.y, world, uintBitsToFloat(hanging.y), needsOwn ? 0.0 : sway.w, spring, fade, seconds);
    return windHangingOffset(firstMotion, secondMotion, alongAndArm.x, swing);
}

#endif
