// The bones' poses as the scene's vertex and mesh shaders read them (ROADMAP phase 21 stage 3c-iii-c;
// render/gpu/bone_poses.h, boneVertexPoses): the bones pass's trees, each plant's tree and the pool, and every pool
// vertex's binding, in the scene set. Included after wind_data.glsl by the passes that pose what they draw.
#ifndef BONE_VERTEX_DATA_GLSL
#define BONE_VERTEX_DATA_GLSL

#include "bone_poses.glsl"
#include "wind_bones.glsl"
#include "wind_data.glsl"

layout(std430, set = 1, binding = SCENE_BONE_TREES_BINDING) readonly buffer SceneBoneTrees {
    BoneTree sceneBoneTrees[];
};

// Two words a held plant: the stamp of the frame that last claimed it, and its tree that frame.
layout(std430, set = 1, binding = SCENE_BONE_PLANT_TREES_BINDING) readonly buffer SceneBonePlantTrees {
    uint scenePlantTrees[];
};

layout(std430, set = 1, binding = SCENE_BONE_POSES_BINDING) readonly buffer SceneBonePoses {
    BonePose sceneBonePoses[];
};

// A word a pool vertex (packSwayBinding: bone low 16 bits, its weight unorm16 high), up to the last bound mesh's.
layout(std430, set = 1, binding = SCENE_SWAY_BINDINGS_BINDING) readonly buffer SceneSwayBindings {
    uint sceneSwayBindings[];
};

// What a drawn vertex is posed by: its tree's first pose (BONE_NO_POSE: it stands still), its binding, and the tree's
// lean in its plant's frame now (xy) and last frame (zw), for the leaves' flutter.
struct BoneDrawn {
    uint firstPose;
    uint binding;
    vec4 leans;
};

/*
* What a bound thing of a plant (BONE_NO_TREE for any other draw) is posed by: its binding (SWAY_UNBOUND none), its tree
* only when claimed and given room this frame (the wind record's stamp, 0 on a frame the bones pass did not run); else
* it stands still.
*/
BoneDrawn boneDrawnBy(uint plant, uint binding) {
    BoneDrawn drawn = BoneDrawn(BONE_NO_POSE, binding, vec4(0.0));
    uint stamp = uint(windField.flags.y);

    if (plant == BONE_NO_TREE || stamp == 0u || binding == SWAY_UNBOUND) {
        return drawn;
    }

    uint tree = boneClaimedTree(scenePlantTrees[2u * plant], scenePlantTrees[2u * plant + 1u], stamp);

    if (tree != BONE_NO_TREE) {
        drawn.firstPose = sceneBoneTrees[tree].firstPose;
        drawn.leans = sceneBoneTrees[tree].leans;
    }

    return drawn;
}

// A vertex's bones (boneDrawnBy) by its place in the vertex pool; most vertices are not bound, so a plant's are read only then.
BoneDrawn boneDrawn(uint plant, uint poolVertex) {
    uint binding = poolVertex < uint(sceneSwayBindings.length()) ? sceneSwayBindings[poolVertex] : SWAY_UNBOUND;
    return boneDrawnBy(binding == SWAY_UNBOUND ? BONE_NO_TREE : plant, binding);
}

bool bonePosed(BoneDrawn drawn) {
    return drawn.firstPose != BONE_NO_POSE;
}

// What a bound vertex blends (BoneVertexPoses): its bone's pose, its parent's (the rest pose for a root), its weight.
struct BoneVertexPoses {
    WindBonePose own;
    WindBonePose parent;
    float weight;
};

WindBonePose bonePoseAt(uint slot) {
    BonePose pose = sceneBonePoses[slot];
    return WindBonePose(pose.turn, pose.shift.xyz);
}

// A posed vertex's two poses, this frame's or last's (boneVertexPoses).
BoneVertexPoses boneVertexPoses(BoneDrawn drawn, bool previous) {
    uint bone = drawn.binding & 0xffffu;
    float weight = float(drawn.binding >> 16u) / 65535.0;
    uint ownSlot = bonePoseSlot(drawn.firstPose, bone, previous);
    int parent = floatBitsToInt(sceneBonePoses[ownSlot].shift.w);
    WindBonePose parentPose = parent < 0 ? windRestPose() : bonePoseAt(bonePoseSlot(drawn.firstPose, uint(parent), previous));
    return BoneVertexPoses(bonePoseAt(ownSlot), parentPose, weight);
}

/*
* Where the bones and the wind move a posed vertex, in the world (as windSwayOfVertex, which the bones replace for it):
* its bones' offset (poses: boneVertexPoses' for the same frame, previous; position in its mesh's frame, posed, then
* turned and scaled by model), plus its leaves' flutter as the sway's (the tree's lean, its height share in sway); a
* hanging vertex (a climber) moves its own anchors as its bones and swings below as before.
*/
vec3 boneSwayOfVertex(BoneDrawn drawn, BoneVertexPoses poses, mat4 model, vec4 sway, uvec4 hanging, int vertex, vec3 position, vec3 world, vec3 eye,
                      bool previous) {
    vec3 own = mat3(model) * (windBlendedPlace(poses.own, poses.parent, poses.weight, position) - position);
    float seconds = previous ? windField.sway.w : windField.wind.w;

    if (hanging.x == 0u) {
        float amount = windFlutterAmount(sway.w, previous ? drawn.leans.zw : drawn.leans.xy, position.y, sway.y);
        return own + windFlutterOffset(world, amount, windField.flutters[int(sway.x)].xy, seconds);
    }

    return windHangingSway(hanging, vertex, windHangingAnchors(hanging, vertex), sway, own, world, eye, seconds, previous);
}

#endif
