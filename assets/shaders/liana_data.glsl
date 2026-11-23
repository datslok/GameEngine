// The lianas the GPU bends (scene/liana_shape.h): their records, and a template vertex (the tube's or the ivy's) placed
// and swayed from its liana's record. Included by the scene's vertex shaders after wind_data.glsl's declarations.
#ifndef LIANA_DATA_GLSL
#define LIANA_DATA_GLSL

#include "bone_vertex_data.glsl"
#include "drawn_objects.glsl"
#include "liana.glsl"
#include "wind_data.glsl"
#include "render/gpu/liana_trees_shared.h"

// Every liana's record, slot by slot (FrameDescription::lianaShapes; one unused without).
layout(std430, set = 1, binding = 41) readonly buffer LianaShapes {
    LianaShape lianaShapes[];
};

// Each liana anchor's tree's plant this frame, two a record (render/vulkan/vulkan_liana_trees.h; one unused without).
layout(std430, set = 1, binding = SCENE_LIANA_TREE_PLANTS_BINDING) readonly buffer LianaTreePlants {
    uint lianaTreePlants[];
};

// A vertex of a liana's draw placed: its record and its number; its offset from the first hold, the offset where it
// sways (itself on the tube, its card's stem for the ivy, so a card swings whole); its normal, tangent, texture place,
// share along and arm.
struct LianaDrawnVertex {
    LianaShape shape;
    uint record;
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
    drawn.record = (named & ~LIANA_IVY_BIT) - 1u;
    drawn.shape = lianaShapes[drawn.record];

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

/*
* How a liana's anchor on a tree moves, this frame or last (previous): as its tree's bark at the hold (the hold's binding
* to the tree's bones, on the plant liana_trees.comp found the tree to be), while that tree is posed; not at all while
* it is not, nor for a rope's end in the ground (unbound).
*/
vec3 lianaAnchorMotion(LianaShape shape, uint record, uint end, bool previous) {
    uint anchor = 2u * record + end;
    uint plant = anchor < uint(lianaTreePlants.length()) ? lianaTreePlants[anchor] : BONE_NO_TREE;
    BoneDrawn drawn = boneDrawnBy(plant, end == 0u ? shape.anchors.x : shape.anchors.y);

    if (!bonePosed(drawn)) {
        return vec3(0.0);
    }

    BoneVertexPoses poses = boneVertexPoses(drawn, previous);
    PlacedPlant tree = scenePlacedPlants[plant];
    vec3 hold = end == 0u ? shape.from.xyz : shape.to.xyz;
    return windPlantPointOffset(poses.own, poses.parent, poses.weight, hold, tree.foot, tree.scale, tree.turnRadians);
}

/*
* Where the wind moves a liana's placed vertex, this frame or last (previous), as a hanging vertex between two anchors
* (windSwayOfVertex's): its first hold moving with its first tree's bark there, its far end with its second's (or
* fixed), blended by its share along, and swinging by its arm below the chord on the hanging spring of its deepest sag,
* where it sways. sway and swing its instance's (its WindSway); eye the camera. Nothing while nothing sways.
*/
vec3 lianaSwayOffset(LianaDrawnVertex bent, vec4 sway, float swing, vec3 eye, bool previous) {
    if (sway.x < 0.0 || windField.sway.x == 0.0) {
        return vec3(0.0);
    }

    LianaShape shape = bent.shape;
    vec3 world = shape.from.xyz + bent.swaysAt;
    float seconds = previous ? windField.sway.w : windField.wind.w;
    vec3 first = lianaAnchorMotion(shape, bent.record, 0u, previous);
    vec3 second = lianaAnchorMotion(shape, bent.record, 1u, previous);
    vec2 spring = windChainSpringAt(world.xz, shape.curve.z, previous);
    float fade = windSwayFade(distance(world, eye), windField.sway.y, windField.sway.z);
    vec3 swingOffset = windSwingOffset(bent.arm, world, swing, sway.w, spring, fade, windField.flutters[WIND_CATEGORY_HANGING].xy, seconds);
    return windHangingOffset(first, second, bent.along, swingOffset);
}

#endif
