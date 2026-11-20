// The GPU scene's objects, as the scene's vertex shaders read them (render/gpu/gpu_scene.h). Resource declarations and
// drawnInstance, included by the scene's vertex shaders.
#ifndef INSTANCES_GLSL
#define INSTANCES_GLSL

#include "render/gpu/instance_shared.h"
#include "instance_data.glsl"

// Every object in the frame: instance i is the frame's draw i.
layout(std430, set = 1, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

// Which instance each drawn copy is: the main pass's run of entries, then each shadow tile's. An instanced draw's
// gl_InstanceIndex counts from its batch's first entry. An entry's top bit (DRAW_LIST_THINNED_BIT) marks an instance the view
// draws thinned (card_thinning_data.glsl), the bits below it its level fade (level_fade.glsl); drawnInstance takes them off.
layout(std430, set = 1, binding = 10) readonly buffer DrawList {
    uint drawList[];
};

uint drawnInstance(uint entry) {
    return entry & DRAW_LIST_INSTANCE_MASK;
}

#endif
