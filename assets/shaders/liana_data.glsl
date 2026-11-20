// The lianas the GPU bends (scene/liana_shape.h): their records, and a template vertex (the tube's or the ivy's) placed
// and swayed from its liana's record. Included by the scene's vertex shaders after wind_data.glsl's declarations.
#ifndef LIANA_DATA_GLSL
#define LIANA_DATA_GLSL

#include "liana.glsl"
#include "wind_data.glsl"

// Every liana's record, slot by slot (FrameDescription::lianaShapes; one unused without).
layout(std430, set = 1, binding = 41) readonly buffer LianaShapes {
    LianaShape lianaShapes[];
};

/*
* Where the wind moves a liana's vertex, this frame or last (previous), as a hanging vertex between two anchors
* (windSwayOfVertex's): its first hold moving with its first tree's crown, its far end with its second's (or fixed),
* blended by its share along, and swinging by its arm below the chord on the hanging spring of its deepest sag. sway and
* swing its instance's (its WindSway); world where it is before swaying, eye the camera. Nothing while nothing sways.
*/
vec3 lianaSwayOffset(LianaShape shape, vec4 sway, float swing, float along, float arm, vec3 world, vec3 eye, bool previous) {
    if (sway.x < 0.0 || windField.sway.x == 0.0) {
        return vec3(0.0);
    }

    float seconds = previous ? windField.sway.w : windField.wind.w;
    vec3 first = windTreeAnchorMotion(lianaWindAnchor(shape, false), eye, seconds, previous);
    vec3 second = shape.trees.y > 0.0 ? windTreeAnchorMotion(lianaWindAnchor(shape, true), eye, seconds, previous) : vec3(0.0);
    vec2 spring = windChainSpringAt(world.xz, shape.curve.z, previous);
    float fade = windSwayFade(distance(world, eye), windField.sway.y, windField.sway.z);
    vec3 swingOffset = windSwingOffset(arm, world, swing, sway.w, spring, fade, windField.flutters[WIND_CATEGORY_HANGING].xy, seconds);
    return windHangingOffset(first, second, along, swingOffset);
}

// A vertex of a liana's draw placed: its record; its offset from the first hold, the offset where it sways (itself on the
// tube, its card's stem for the ivy, so a card swings whole); its normal, tangent, texture place, share along and arm.
struct LianaDrawnVertex {
    LianaShape shape;
    vec3 offset;
    vec3 swaysAt;
    vec3 normal;
    vec3 tangent;
    vec2 uv;
    float along;
    float arm;
};

/*
* A draw's template vertex placed from the liana it names (the instance's hanging.w: one past its record, with
* LIANA_IVY_BIT for its ivy): the tube's (position: share along, cos and sin round; u round) bent onto its curve, or an
* ivy card's corner (lianaLeafTemplateVertex) grown for its distance from eye, the camera in every view so all agree.
*/
LianaDrawnVertex lianaDrawnVertex(uint named, vec3 position, vec2 uv, vec3 eye) {
    LianaDrawnVertex drawn;
    drawn.shape = lianaShapes[(named & ~LIANA_IVY_BIT) - 1u];

    if ((named & LIANA_IVY_BIT) != 0u) {
        LianaLeafVertex leaf = lianaLeafTemplateVertex(drawn.shape, uv, eye);
        drawn.offset = leaf.offset;
        drawn.swaysAt = leaf.stem;
        drawn.normal = leaf.normal;
        drawn.tangent = leaf.tangent;
        drawn.uv = leaf.uv;
        drawn.along = leaf.along;
        drawn.arm = leaf.arm;
        return drawn;
    }

    LianaVertex tube = lianaVertex(drawn.shape, position.x, uv.x, position.y, position.z);
    drawn.offset = tube.offset;
    drawn.swaysAt = tube.offset;
    drawn.normal = tube.normal;
    drawn.tangent = tube.tangent;
    drawn.uv = tube.uv;
    drawn.along = tube.along;
    drawn.arm = tube.arm;
    return drawn;
}

#endif
