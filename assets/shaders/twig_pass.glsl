// What the twig passes share (render/vulkan/vulkan_twig_drawing.h): the camera's placed plants, the twig tables, the
// sources gathered and the regions written.
#ifndef TWIG_PASS_GLSL
#define TWIG_PASS_GLSL

#include "placed_drawing.glsl"
#include "twig_drawing.glsl"

// The bones (bone_cull_data.glsl): each plant's tree, the trees, the pool, the twigs' bindings.
#define BONE_CULL_BINDING TWIG_BONE_BINDING
#include "bone_cull_data.glsl"

// The camera's view as the mesh cluster passes take it (its sway, its pixel error).
layout(std430, set = 0, binding = 0) readonly buffer TwigView {
    CameraCull view;
};

// The camera's placed plants: its commands by placed group (the camera is the placed drawing's first view), its
// entries, each part's instance and record in its plant's frame, the slots' plants.
layout(std430, set = 0, binding = 1) readonly buffer PlacedCommands {
    DrawCommand placedCommands[];
};

layout(std430, set = 0, binding = 2) readonly buffer PlacedEntries {
    uvec2 placedEntries[];
};

layout(std430, set = 0, binding = 3) readonly buffer PlacedPartInstances {
    InstanceData placedPartInstances[];
};

layout(std430, set = 0, binding = 4) readonly buffer PlacedParts {
    PlacedPart placedParts[];
};

layout(std430, set = 0, binding = 5) readonly buffer PlacedPlants {
    PlacedPlant placedPlants[];
};

// The placed groups the twig parts' level slots draw through (x), their count in the first word's place before them.
layout(std430, set = 0, binding = 6) readonly buffer TwigGroups {
    uint twigGroupCount;
    uint twigGroups[];
};

// Each twig set (GpuTwigSet): its bricks (x first, y count), its far nodes' first (z; TWIG_FAR_NO_NODE without), its first cell shape (w).
layout(std430, set = 0, binding = 7) readonly buffer TwigSets {
    uvec4 twigSets[];
};

layout(std430, set = 0, binding = 8) readonly buffer TwigBricks {
    TwigBrick twigBricks[];
};

layout(std430, set = 0, binding = 9) readonly buffer Twigs {
    Twig twigs[];
};

layout(std430, set = 0, binding = 10) readonly buffer TwigPrototypes {
    TwigPrototype twigPrototypes[];
};

layout(std430, set = 0, binding = 11) readonly buffer TwigLevels {
    TwigLevel twigLevels[];
};

// The sources gathered: each a placed entry's place and its set (x y), their count first.
layout(std430, set = 0, binding = 12) buffer TwigSources {
    uint twigSourceCount;
    uint twigSourcePadding;
    uvec2 twigSources[];
};

// The cull pass's dispatch: the most bricks a set has, the sources, 1.
layout(std430, set = 0, binding = 13) buffer TwigDispatch {
    uint twigDispatch[3];
};

// Each region's entries written (may pass the room; TWIG_REGIONS of TWIG_REGION_WORDS, the far region last), the room
// (x of the uvec4 after), then the cull pass's tally (TWIG_TALLY_*: F4's TWIGS line).
layout(std430, set = 0, binding = 14) buffer TwigRegions {
    uint twigCounts[TWIG_REGION_WORDS];
    uvec4 twigRoom;
    uint twigTally[TWIG_TALLY_WORDS];
};

// The twigs drawn: region r's from r times the room, each (placed entry place, twig, its level's first cluster, 0); the
// far region's (placed entry place, 0, the far cluster with TWIG_FAR_CLUSTER_BIT, 0).
layout(std430, set = 0, binding = 15) writeonly buffer TwigEntries {
    uvec4 twigEntries[];
};

// The far tree (render/gpu/twig_records.h, packTwigSets): each set's nodes (brick k's node its record's z plus k) and
// their levels (each a run of far leaf clusters).
layout(std430, set = 0, binding = TWIG_FAR_BINDING) readonly buffer TwigFarNodes {
    TwigFarNode twigFarNodes[];
};

layout(std430, set = 0, binding = TWIG_FAR_BINDING + 1) readonly buffer TwigFarLevels {
    TwigFarLevel twigFarLevels[];
};

// The most bricks any twig set has (the dispatch's x), the most sources the room takes, the frame's bones stamp
// (GpuWindField::flags[1]: 0 when the bones pass did not run), and whether far bricks hand over to their far levels
// (Passes "Far leaves").
layout(push_constant) uniform TwigPush {
    uint mostBricks;
    uint mostSources;
    uint boneStamp;
    uint farLeaves;
} push;

#endif
