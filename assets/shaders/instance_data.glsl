// One object of the GPU scene (render/gpu/gpu_scene.h, GpuInstance), the struct alone: instances.glsl declares the
// buffers holding them, placed_drawing.glsl builds one from a placed plant.
#ifndef INSTANCE_DATA_GLSL
#define INSTANCE_DATA_GLSL

// One object, the same for every view. Must match GpuInstance.
struct InstanceData {
    mat4 model;        // places the vertex in the world
    mat4 normalMatrix; // inverse transpose of the model's 3x3 part, computed once per object on the CPU (zero if squashed flat)
    vec4 handedness;   // x: -1 if the model matrix mirrors space, else +1; y: its scale (uniformScale)
    uvec4 material;    // x: the material's number in the material buffer, y: the mesh's (for ray hits), z: its terrain patch's one past (0 none), w: its kind (instance_shared.h)
    vec4 sway;         // how it sways in the wind (render/gpu/wind.h, packSway): category (-1 still), height, bend, flutter
    uvec4 hanging;     // x: one past its mesh's first hanging vertex (0 none), y: its swing's bits, z: one past its mesh's card thinning header (0 none), w: one past its liana's record (liana_data.glsl, 0 none)
    vec4 levelSphere;  // the sphere its level of detail is chosen by, in the world: xyz centre, w radius (below 0 unknown)
};

#endif
