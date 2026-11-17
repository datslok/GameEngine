// The GPU scene's objects, as the scene's vertex shaders read them (render/gpu/gpu_scene.h). Resource declarations and
// drawnInstance, included by the scene's vertex shaders.
#ifndef INSTANCES_GLSL
#define INSTANCES_GLSL

#include "render/gpu/instance_shared.h"

// One object, the same for every view. Must match GpuInstance.
struct InstanceData {
    mat4 model;        // places the vertex in the world
    mat4 normalMatrix; // inverse transpose of the model's 3x3 part, computed once per object on the CPU (zero if squashed flat)
    vec4 handedness;   // x: -1 if the model matrix mirrors space, else +1
    uvec4 material;    // x: the material's number in the material buffer, y: the mesh's (for ray hits)
    vec4 sway;         // how it sways in the wind (render/gpu/wind.h, packSway): category (-1 still), height, bend, flutter
    uvec4 hanging;     // x: one past its mesh's first hanging vertex (0 none), y: its swing's bits, z: one past its mesh's card thinning header (0 none)
    vec4 levelSphere;  // the sphere its level of detail is chosen by, in the world: xyz centre, w radius (below 0 unknown)
};

// Every object in the frame: instance i is the frame's draw i.
layout(std430, set = 1, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

// Which instance each drawn copy is: the main pass's run of entries, then each shadow tile's. An instanced draw's
// gl_InstanceIndex counts from its batch's first entry. An entry's top bit (DRAW_LIST_THINNED_BIT) marks an instance the view
// draws thinned (card_thinning_data.glsl); drawnInstance takes it off.
layout(std430, set = 1, binding = 10) readonly buffer DrawList {
    uint drawList[];
};

uint drawnInstance(uint entry) {
    return entry & ~DRAW_LIST_THINNED_BIT;
}

#endif
