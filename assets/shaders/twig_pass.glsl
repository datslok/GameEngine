// What the twig passes share (render/vulkan/vulkan_twig_drawing.h): the camera's placed plants, the twig tables, the
// sources gathered and the regions written.
#ifndef TWIG_PASS_GLSL
#define TWIG_PASS_GLSL

#include "placed_drawing.glsl"
#include "twig_drawing.glsl"

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

// Each twig set's bricks (x first, y count), as the table's sets.
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

// Each region's entries written (may pass the room), and the room (x of the second).
layout(std430, set = 0, binding = 14) buffer TwigRegions {
    uint twigCounts[TWIG_MOST_LEVEL_CLUSTERS];
    uvec4 twigRoom;
};

// The twigs drawn: region r's from r times the room, each (placed entry place, twig, its level's first cluster, 0).
layout(std430, set = 0, binding = 15) writeonly buffer TwigEntries {
    uvec4 twigEntries[];
};

// The most bricks any twig set has (the dispatch's x), and the most sources the room takes.
layout(push_constant) uniform TwigPush {
    uint mostBricks;
    uint mostSources;
} push;

#endif
