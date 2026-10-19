// The frame's uniform block, the same for every vertex and pixel. Must match GpuFrameData in render/gpu/gpu_scene.h.
// Resource declarations only (no functions), included by the scene's passes.
#ifndef FRAME_DATA_GLSL
#define FRAME_DATA_GLSL

layout(std140, set = 1, binding = 0) uniform FrameData {
    mat4 relativeViewProjection; // the camera's view-projection, for points measured from the camera
    vec4 cameraOrigin;           // xyz: the camera's position in the world
    vec4 ambient;
    ivec4 counts;                // x: directional lights used, y: point lights used, z: spotlights used
    vec4 atlasTexel;             // xy: one shadow atlas texel's size in atlas coordinates
};

#endif
