// What the bones passes share (render/vulkan/vulkan_bone_posing.h): their own set (set 2, after the textures' and the
// scene's, which the pose pass reads the wind from) and their push constants.
#ifndef BONE_PASS_GLSL
#define BONE_PASS_GLSL

#include "bone_poses.glsl"
#include "placed_drawing.glsl"

// Every placed view's commands, a view's groups after the view before's, and every view's entries (plant with the
// draw list's bits, part).
layout(std430, set = 2, binding = BONE_COMMANDS_BINDING) readonly buffer BoneCommands {
    DrawCommand placedCommands[];
};

layout(std430, set = 2, binding = BONE_ENTRIES_BINDING) readonly buffer BoneEntries {
    uvec2 placedEntries[];
};

// The placed table's parts (unused1 a part's skeleton plus 1).
layout(std430, set = 2, binding = BONE_PARTS_BINDING) readonly buffer BoneParts {
    PlacedPart placedParts[];
};

layout(std430, set = 2, binding = BONE_BONES_BINDING) readonly buffer BoneBones {
    SwayBoneRecord swayBones[];
};

layout(std430, set = 2, binding = BONE_SKELETONS_BINDING) readonly buffer BoneSkeletons {
    SwaySkeletonRecord swaySkeletons[];
};

// Each chain's first bone within its skeleton and its bone count; each skeleton's levels' first chains.
layout(std430, set = 2, binding = BONE_CHAINS_BINDING) readonly buffer BoneChains {
    uvec2 swayChains[];
};

layout(std430, set = 2, binding = BONE_LEVEL_STARTS_BINDING) readonly buffer BoneLevelStarts {
    uint swayLevelStarts[];
};

// The frame's work: the swaying views (placed view numbers) then the groups of parts moved by bones, and how many
// groups a view has in the commands.
layout(std430, set = 2, binding = BONE_WORK_BINDING) readonly buffer BoneWork {
    uint swayingViewCount;
    uint boneGroupCount;
    uint groupsAView;
    uint workUnused;
    uint workList[];
};

// The counts and the pose pass's dispatch (BONE_HEADER_*).
layout(std430, set = 2, binding = BONE_HEADER_BINDING) buffer BoneHeader {
    uint boneHeader[BONE_HEADER_WORDS];
};

layout(std430, set = 2, binding = BONE_TREES_BINDING) buffer BoneTrees {
    BoneTree boneTrees[];
};

// Two words a held plant: the stamp of the frame that last claimed it, and its tree that frame (BONE_NO_TREE none).
layout(std430, set = 2, binding = BONE_PLANT_TREES_BINDING) buffer BonePlantTrees {
    uint plantTrees[];
};

// The pool: a tree's bone b's pose this frame at its first pose + 2 b, last frame's after it. Coherent: a workgroup's
// threads read the poses others wrote at the level before, after a barrier.
layout(std430, set = 2, binding = BONE_POSES_BINDING) coherent buffer BonePoses {
    BonePose bonePoses[];
};

layout(std430, set = 2, binding = BONE_PLANTS_BINDING) readonly buffer BonePlants {
    PlacedPlant placedPlants[];
};

// Each placed part's instance in its plant's frame (its sway: the lean part's bend).
layout(std430, set = 2, binding = BONE_PART_INSTANCES_BINDING) readonly buffer BonePartInstances {
    InstanceData placedPartInstances[];
};

// The frame's stamp (never 0), the pool's bones, the sway's fade end and the eye.
layout(push_constant) uniform BonePush {
    uint stamp;
    uint poolBones;
    float fadeEnd;
    uint unused;
    vec4 eye;
} push;

#endif
