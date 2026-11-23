#ifndef BONE_POSES_SHARED_H
#define BONE_POSES_SHARED_H

// The bones pass (render/gpu/bone_poses.h, bone_poses.glsl, bones_claim.comp and bones_pose.comp), numbers shared by
// C++ and GLSL. Only #defines of plain numbers, since GLSL includes this file too.

// Threads a workgroup of both passes: the claim's over a group's entries, the pose's over a level's chains.
#define BONE_GROUP_SIZE 64

// The most trees posed a frame (a tree record each); a tree past them stands still.
#define BONE_MOST_TREES 65536

// A tree's first pose when the pool had no room for its bones: it stands still. And no tree (a plant not claimed).
#define BONE_NO_POSE 0xFFFFFFFFu
#define BONE_NO_TREE 0xFFFFFFFFu

// A skeleton with no part to take its lean from (none of its parts sways as a tree): its trees stand still.
#define BONE_NO_PART 0xFFFFFFFFu

// The header's words: trees claimed, bones asked for (fitting or not), trees left still, then the pose pass's
// indirect dispatch (x the trees, y and z 1).
#define BONE_HEADER_TREES 0
#define BONE_HEADER_BONES_ASKED 1
#define BONE_HEADER_TREES_STILL 2
#define BONE_HEADER_DISPATCH 4
#define BONE_HEADER_WORDS 8

// The passes' own set (set 2, after the textures' and the scene's): the placed plants' commands and entries, the parts,
// the bones, skeletons, chains and levels' starts, the swaying views and skeleton groups, the header, the trees, each
// plant's tree, the poses, the slots' plants and each placed part's instance in its plant's frame.
#define BONE_COMMANDS_BINDING 0
#define BONE_ENTRIES_BINDING 1
#define BONE_PARTS_BINDING 2
#define BONE_BONES_BINDING 3
#define BONE_SKELETONS_BINDING 4
#define BONE_CHAINS_BINDING 5
#define BONE_LEVEL_STARTS_BINDING 6
#define BONE_WORK_BINDING 7
#define BONE_HEADER_BINDING 8
#define BONE_TREES_BINDING 9
#define BONE_PLANT_TREES_BINDING 10
#define BONE_POSES_BINDING 11
#define BONE_PLANTS_BINDING 12
#define BONE_PART_INSTANCES_BINDING 13
#define BONE_PASS_BINDINGS 14

// The scene set's bindings the draws read the poses through (set 1, render/vulkan/scene_layout.h): the trees, each
// plant's tree, the pool; and every pool vertex's binding to its bones (render/gpu/bone_records.h). The frame's stamp
// rides in the wind's record (GpuWindField::flags[1], 0 when the passes do not run).
#define SCENE_BONE_TREES_BINDING 50
#define SCENE_BONE_PLANT_TREES_BINDING 51
#define SCENE_BONE_POSES_BINDING 52
#define SCENE_SWAY_BINDINGS_BINDING 48

#endif
