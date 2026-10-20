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
    ivec4 clusterTiles;          // x: tiles across, y: tiles down, z: depth slices
    vec4 clusterScreen;          // xy: the screen in pixels, z: half the view's width at depth 1, w: its half height
    vec4 clusterSlicing;         // x: slice scale, y: slice bias
    vec4 viewRight;              // xyz: the view's axes in the world
    vec4 viewUp;
    vec4 viewForward;
    ivec4 shadingOptions;        // x: 1 shades from the clusters' lists, y: 1 shows their light counts (the heatmap)
    vec4 cascadeOptions;         // x: the sun cascades' blend band (a share of a radius), y: 1 dithers between them instead
    vec4 probeShading;           // x: the light probes' self-shadow bias setting, y: the share of probe light a probe ray's hit passes on
    ivec4 probeAtlas;            // xy: the probe atlases' columns and rows, w: how many cascades
    ivec4 probeLights;           // x: point lights listed for probe rays' hits, y: spotlights, z: how many each hit keeps
};

#endif
