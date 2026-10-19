// The GPU scene's objects, as the scene's vertex shaders read them (render/gpu/gpu_scene.h). Resource declarations only
// (no functions), included by triangle.vert and shadow.vert.
#ifndef INSTANCES_GLSL
#define INSTANCES_GLSL

// One object, the same for every view. Must match GpuInstance.
struct InstanceData {
    mat4 model;        // places the vertex in the world
    mat4 normalMatrix; // inverse transpose of the model's 3x3 part, computed once per object on the CPU (zero if squashed flat)
    vec4 handedness;   // x: -1 if the model matrix mirrors space, else +1
    uvec4 material;    // x: the material's number in the material buffer, y: the mesh's (for ray hits)
};

// Every object in the frame: instance i is the frame's draw i.
layout(std430, set = 1, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

// Which instance each drawn copy is: the main pass's run of entries, then each shadow tile's. An instanced draw's
// gl_InstanceIndex counts from its batch's first entry.
layout(std430, set = 1, binding = 10) readonly buffer DrawList {
    uint drawList[];
};

#endif
