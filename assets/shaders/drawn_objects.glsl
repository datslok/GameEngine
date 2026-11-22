// What a scene vertex shader's instance draws (render/gpu/gpu_scene.h, render/gpu/placed_drawing.h): a frame's draw
// through the draw list, or, for a draw from PLACED_INSTANCE_BASE, a placed plant's part built from the plant and its
// part's instance in the plant's frame. Resource declarations and drawnObject, included by the scene's vertex shaders.
#ifndef DRAWN_OBJECTS_GLSL
#define DRAWN_OBJECTS_GLSL

#include "instances.glsl"
#include "placed_drawing.glsl"
#include "twig_record.glsl"

// The planting cells' slots' plants (render/gpu/planting_records.h; one unused without).
layout(std430, set = 1, binding = SCENE_PLACED_PLANTS_BINDING) readonly buffer ScenePlacedPlants {
    PlacedPlant scenePlacedPlants[];
};

// Each placed part's instance in its plant's frame, in the placed table's part order (one unused without).
layout(std430, set = 1, binding = SCENE_PLACED_PARTS_BINDING) readonly buffer ScenePlacedParts {
    InstanceData scenePlacedParts[];
};

// The camera's placed entries: the plant with the draw list's bits, and the part (one unused without).
layout(std430, set = 1, binding = SCENE_PLACED_ENTRIES_BINDING) readonly buffer ScenePlacedEntries {
    uvec2 scenePlacedEntries[];
};

// The camera's twig entries (a placed entry's place, the twig, its level's first cluster) and the twigs (one unused
// each without; render/gpu/twig_records.h): the mesh shaders' twig instances.
layout(std430, set = 1, binding = SCENE_TWIG_ENTRIES_BINDING) readonly buffer SceneTwigEntries {
    uvec4 sceneTwigEntries[];
};

layout(std430, set = 1, binding = SCENE_TWIGS_BINDING) readonly buffer SceneTwigs {
    Twig sceneTwigs[];
};

/*
* An instance drawn and its entry's bits (thinned, fade), whichever kind of draw it came from; for a twig its part's
* instance (its plant and part: the twig sways with its tree) and the twig's matrix, which places the prototype's
* vertices in the part's frame first (shadeVertex).
*/
struct DrawnObject {
    InstanceData instance;
    uint entry;
    bool twig;
    mat4 twigMatrix;
};

/*
* The instance an instanced draw's copy draws: instanceIndex counts from the draw's first instance (baseInstance). The
* branch is the same for a whole draw, so the copies of one draw never diverge on it.
*/
DrawnObject drawnObject(uint instanceIndex, uint baseInstance) {
    DrawnObject drawn;
    drawn.twig = false;
    drawn.twigMatrix = mat4(1.0);

    // A twig (the mesh shaders only: its entry is the instance and the base alike): its part as its placed entry's.
    if (baseInstance >= TWIG_INSTANCE_BASE) {
        uvec4 twig = sceneTwigEntries[instanceIndex - TWIG_INSTANCE_BASE];
        uvec2 placed = scenePlacedEntries[twig.x];
        drawn.instance = placedInstance(scenePlacedParts[placed.y], scenePlacedPlants[drawnInstance(placed.x)]);
        drawn.entry = placed.x;
        drawn.twig = true;
        drawn.twigMatrix = twigMatrix(sceneTwigs[twig.y]);
        return drawn;
    }

    if (baseInstance >= PLACED_INSTANCE_BASE) {
        uvec2 placed = scenePlacedEntries[instanceIndex - PLACED_INSTANCE_BASE];
        drawn.instance = placedInstance(scenePlacedParts[placed.y], scenePlacedPlants[drawnInstance(placed.x)]);
        drawn.entry = placed.x;
        return drawn;
    }

    drawn.entry = drawList[instanceIndex];
    drawn.instance = instances[drawnInstance(drawn.entry)];
    return drawn;
}

#endif
