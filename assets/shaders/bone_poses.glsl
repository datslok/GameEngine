// The bones pass's records and maths (render/gpu/bone_poses.h, which documents each function and mirrors it), included
// with #include "bone_poses.glsl".

#ifndef BONE_POSES_GLSL
#define BONE_POSES_GLSL

#include "render/gpu/bone_poses_shared.h"
#include "scene/sway_bones_shared.h"

// A bone of a skeleton (GpuSwayBone): pivot and parent within its skeleton (-1 a root), tip and radius, its chain's
// first bone, length and radius at its start, its level.
struct SwayBoneRecord {
    vec3 pivot;
    int parent;
    vec3 tip;
    float radius;
    uint chainStart;
    float chainLength;
    float chainRadius;
    uint level;
};

// A skeleton (GpuSwaySkeleton): its bones' run, height and foot radius, its chain levels' starts, its lean's part, its reach.
struct SwaySkeletonRecord {
    uint firstBone;
    uint boneCount;
    float height;
    float footRadius;
    uint firstLevel;
    uint levelCount;
    uint leanPart;
    float reach;
};

// A posed tree (GpuBoneTree): its plant, skeleton, first pose (BONE_NO_POSE still), its move this frame (boneTreeMove),
// its lean now (xy) and last frame's (zw).
struct BoneTree {
    uint plant;
    uint skeleton;
    uint firstPose;
    float move;
    vec4 leans;
};

// A bone's pose in the pool (GpuBonePose): its turn, its shift (xyz) and its parent within its skeleton (w's bits as
// an int, -1 a root).
struct BonePose {
    vec4 turn;
    vec4 shift;
};

// A lean along the ground in a plant's own frame (windLeanInPlant).
vec2 windLeanInPlant(vec2 lean, float turnRadians) {
    float c = cos(turnRadians);
    float s = sin(turnRadians);
    return vec2(lean.x * c - lean.y * s, lean.x * s + lean.y * c);
}

// Whether a tree is posed: its foot within the sway's reach (boneTreeWanted).
bool boneTreeWanted(vec3 foot, vec3 eye, float fadeEnd) {
    return length(foot - eye) < fadeEnd;
}

// A tree's first pose, or BONE_NO_POSE when its bones do not fit the pool (boneTreeFirstPose); no overflow in 32 bits.
uint boneTreeFirstPose(uint askedBefore, uint boneCount, uint poolBones) {
    return askedBefore <= poolBones && boneCount <= poolBones - askedBefore ? 2u * askedBefore : BONE_NO_POSE;
}

// A tree's seed for its branches' phases: its plant's pick's bits (boneTreeSeed).
uint boneTreeSeed(float pick) {
    return floatBitsToUint(pick);
}

// The tree a plant's word pair holds this frame, BONE_NO_TREE unless claimed by this frame's stamp (never 0) (boneClaimedTree).
uint boneClaimedTree(uint claimStamp, uint claimTree, uint frameStamp) {
    return frameStamp != 0u && claimStamp == frameStamp ? claimTree : BONE_NO_TREE;
}

// A bone's place in the pool, last frame's after this frame's (bonePoseSlot).
uint bonePoseSlot(uint firstPose, uint bone, bool previous) {
    return firstPose + 2u * bone + (previous ? 1u : 0u);
}

#endif
