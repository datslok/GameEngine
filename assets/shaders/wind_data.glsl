// The wind force field as the scene's shaders read it (render/gpu/wind.h): its record and its springs, this frame's and
// last frame's (for motion vectors), and what they do to a vertex. Included by the passes that sway what they draw.
#ifndef WIND_DATA_GLSL
#define WIND_DATA_GLSL

#include "wind.glsl"

layout(std430, set = 1, binding = 37) readonly buffer WindFieldRecord {
    WindField windField;
};

// Each texel a cell's springs, layer cascade * WIND_SPRINGS + spring: xy displacement, zw velocity. Sampled with
// wrapping and linear filtering, so a place reads its cell's spring at place / (cell * WIND_CELLS) whichever window
// texel holds it (cell c's middle, (c + 0.5) cells along, at texel c's middle).
layout(set = 1, binding = 38) uniform sampler2DArray windSprings;
layout(set = 1, binding = 39) uniform sampler2DArray windPreviousSprings;

// Every hanging mesh's vertices (render/gpu/wind.h, packHangingVertex), three words each: the anchors (first in the
// low 16 bits, WIND_ANCHOR_FIXED or WIND_ANCHOR_OWN), the share along and the arm as half floats, the swing's length as
// a half float.
layout(std430, set = 1, binding = 40) readonly buffer HangingVertices {
    uint hangingWords[];
};

/*
* One of the field's springs (a category's, or a hanging band's) at a place on the ground, this frame's or last frame's
* (in last frame's windows): the near window's, passing into the far one's over its last cells (windWindowShare),
* nothing outside both.
*/
vec2 windSpringAt(vec2 place, int springIndex, bool previous) {
    vec2 spring = vec2(0.0);
    float left = 1.0;

    for (int cascade = 0; cascade < WIND_CASCADES; ++cascade) {
        ivec4 window = windField.windows[cascade];
        float share = windWindowShare(place, previous ? window.zw : window.xy, windField.cells[cascade].x);

        if (share > 0.0) {
            vec3 coordinate = vec3(place * windField.cells[cascade].y, float(cascade * WIND_SPRINGS + springIndex));
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
    return windSwayOffset(fromFoot, position.y, world, sway, scale, spring, fade, windField.flutters[category].xy, seconds);
}

// How an anchor on a tree moves: as the tree does there (windAnchorOffset), its spring and fade at the tree's foot.
vec3 windTreeAnchorMotion(WindAnchorRecord record, vec3 eye, float seconds, bool previous) {
    vec2 spring = windSpringAt(record.foot.xz, WIND_CATEGORY_TREES, previous);
    float fade = windSwayFade(distance(record.foot.xyz, eye), windField.sway.y, windField.sway.z);
    return windAnchorOffset(record, spring, fade, seconds);
}

// How an anchor of a hanging vertex moves: not at all when fixed, else as its own object sways at the vertex (own).
vec3 windAnchorMotion(uint anchor, vec3 own) {
    return anchor == uint(WIND_ANCHOR_FIXED) ? vec3(0.0) : own;
}

// A hanging vertex's spring at a place: the hanging bands' springs blended by its swing's length (windChainShares), so
// a long strand swings slower than a short one; bands it has no share of are not read.
vec2 windChainSpringAt(vec2 place, float swingLength, bool previous) {
    vec3 bands = vec3(windField.springs[WIND_SPRING_HANGING_SHORT].x, windField.springs[WIND_SPRING_HANGING_SHORT + 1].x,
                      windField.springs[WIND_SPRING_HANGING_SHORT + 2].x);
    vec3 shares = windChainShares(swingLength, bands);
    vec2 spring = vec2(0.0);

    for (int band = 0; band < WIND_HANGING_BANDS; ++band) {
        if (shares[band] > 0.0) {
            spring += shares[band] * windSpringAt(place, WIND_SPRING_HANGING_SHORT + band, previous);
        }
    }

    return spring;
}

// A hanging vertex's anchors' word (its mesh's first hanging vertex in hanging.x, plus 1): first low, second high.
uint windHangingAnchors(uvec4 hanging, int vertex) {
    return hangingWords[3u * (hanging.x - 1u + uint(vertex))];
}

// Whether either anchor moves as the object's own sway does at the vertex.
bool windAnchorsNeedOwn(uint anchors) {
    return (anchors & 0xffffu) == uint(WIND_ANCHOR_OWN) || (anchors >> 16u) == uint(WIND_ANCHOR_OWN);
}

/*
* A hanging vertex's motion given own, how its object moves at it (its sway, or its bones'): its anchors' motions
* blended by its share along, and its swing on the hanging spring where it is (no flutter when an anchor is its own:
* the own motion fluttered already).
*/
vec3 windHangingSway(uvec4 hanging, int vertex, uint anchors, vec4 sway, vec3 own, vec3 world, vec3 eye, float seconds, bool previous) {
    uint word = 3u * (hanging.x - 1u + uint(vertex));
    vec2 alongAndArm = unpackHalf2x16(hangingWords[word + 1u]);
    float swingLength = unpackHalf2x16(hangingWords[word + 2u]).x;
    bool needsOwn = windAnchorsNeedOwn(anchors);
    vec3 firstMotion = windAnchorMotion(anchors & 0xffffu, own);
    vec3 secondMotion = windAnchorMotion(anchors >> 16u, own);
    vec2 spring = windChainSpringAt(world.xz, swingLength, previous);
    float fade = windSwayFade(distance(world, eye), windField.sway.y, windField.sway.z);
    vec3 swing = windSwingOffset(alongAndArm.y, world, uintBitsToFloat(hanging.y), needsOwn ? 0.0 : sway.w, spring, fade,
                                 windField.flutters[WIND_CATEGORY_HANGING].xy, seconds);
    return windHangingOffset(firstMotion, secondMotion, alongAndArm.x, swing);
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

    uint anchors = windHangingAnchors(hanging, vertex);
    vec3 own = windAnchorsNeedOwn(anchors) ? windOwnSway(model, sway, scale, position, world, eye, seconds, previous) : vec3(0.0);
    return windHangingSway(hanging, vertex, anchors, sway, own, world, eye, seconds, previous);
}

#endif
