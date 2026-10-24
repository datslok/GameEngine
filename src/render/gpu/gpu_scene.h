#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/clusters.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/material.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

/*
* The GPU scene: the frame's objects, materials and lights as arrays in storage buffers, written once per frame, which
* the shaders index themselves: every object of the frame once (its instance), then a draw list of instance numbers, one
* run per view and mesh, each vertex shader reading draw list entry gl_InstanceIndex; an instance carries its material's
* number, a material its textures' numbers. A draw then names only a mesh and a run of the draw list, so every object
* with the same mesh is one instanced draw in each view, and nothing is bound per object. These are the parts that do not touch the
* graphics API: the records' layouts (std430, mirrored in the shaders) and how the frame's draws are grouped.
*/

/*
* One object as the scene's vertex shaders read it from the instance buffer, the same for every view (the camera's and
* every shadow tile's), so it is written once per frame: the world model matrix, the normal matrix and the handedness
* (all column-major), and the number of its material in the frame's material table. Each view multiplies in its own
* matrix on the GPU. Ray tracing's instances (6d) and GPU culling will read the same records.
*/
struct GpuInstance {
    float model[16];        // places the vertex in the world
    float normalMatrix[16]; // inverse transpose of the model's 3x3 part (zero if squashed flat)
    float handedness[4];    // x: -1 if the model matrix mirrors space, else +1; y: its scale (uniformScale), for lengths in model units
    std::uint32_t material[4]; // x: the material's number in the frame's material buffer, y: the mesh's (for ray hits)
};

static_assert(sizeof(GpuInstance) == 160, "GpuInstance must match the shader's InstanceData struct");

// A shadow tile's view-projection (column-major, world space), pushed before its casters are drawn.
struct GpuShadowView {
    float matrix[16];
};

static_assert(sizeof(GpuShadowView) == 64, "GpuShadowView must match shadow.vert's push constants");

/*
* The frame's small uniform block (std140): for the vertex shader the camera's view-projection measured from the camera
* and the camera's position, which make each instance camera-relative (render/gpu/camera_relative.h); for every pixel
* the ambient light, how many lights of each kind the light buffers hold this frame, and the size of one shadow atlas texel;
* and the cluster grid (render/gpu/clusters.h), as clusters.glsl's ClusterGrid, for the main pass and cluster_lights.comp.
*/
struct GpuFrameData {
    float relativeViewProjection[16];
    float cameraOrigin[4];  // xyz
    float ambient[4];
    std::int32_t counts[4]; // x: directional lights, y: point lights, z: spotlights
    float atlasTexel[4];    // xy: one texel's size in atlas coordinates
    std::int32_t clusterTiles[4]; // x: tiles across, y: tiles down, z: depth slices
    float clusterScreen[4];       // xy: the screen in pixels, z: half the view's width at depth 1, w: its half height
    float clusterSlicing[4];      // x: slice scale, y: slice bias
    float viewRight[4];           // xyz: the view's axes in the world
    float viewUp[4];
    float viewForward[4];
    std::int32_t shadingOptions[4]; // x: 1 shades from the clusters' lists, y: 1 shows their light counts (the heatmap)
    float cascadeOptions[4];        // x: the sun cascades' blend band (a share of a radius), y: 1 dithers between them instead
    // The light probes (render/gpu/ddgi_sampling.h, packProbeSampling), all zero while they are not sampled.
    float probeShading[4];          // x: the self-shadow bias setting, y: the share of probe light a probe ray's hit passes on
    std::int32_t probeAtlas[4];     // xy: the probe atlases' columns and rows (ddgi.h, probeAtlasGrid), w: how many cascades
    std::int32_t probeLights[4];    // x: point lights listed for probe rays' hits, y: spotlights, z: how many each hit keeps
    float textureOptions[4];        // x: the surface textures' mip bias (render/gpu/upscaling.h, textureLodBias), y: 1 filters normal maps (Toksvig)
    std::int32_t mirroredLightOptions[4]; // x: 1 when the main pass adds the light mirrors reflect (render/gpu/mirrored_light.h), y: the frame, turning rough mirrors' samples
    std::int32_t ambientOcclusionOptions[4]; // x: 1 when the main pass reads ambient occlusion (render/gpu/ambient_occlusion.h), y: 1 with multiple bounces, z: 1 with specular occlusion
};

static_assert(sizeof(GpuFrameData) == 352, "GpuFrameData must match the shader's FrameData block");

// One object's instance, with the number its material got in the frame's MaterialTable.
GpuInstance packInstance(const DrawItem& draw, std::uint32_t material);

GpuShadowView packShadowView(const Mat4& tileMatrix);

GpuFrameData makeFrameData(const GpuLights& lights, const Mat4& relativeViewProjection, const Vec3& cameraOrigin,
                           const ClusterGrid& clusters, bool clusteredShading, bool clusterHeatmap, const ShadowSettings& shadows = {});

/*
* The frame's materials, one entry per different material: draws carry their materials by value, and many share one (a
* hundred pillars of the same stone), so add() returns the number of an identical entry when there is one.
*/
class MaterialTable {
public:
    std::uint32_t add(const Material& material);
    const std::vector<GpuMaterial>& getEntries() const;

private:
    std::vector<GpuMaterial> entries;

    // Entries by a hash of their bytes; equal hashes are compared in full, so a collision only costs a comparison.
    std::unordered_multimap<std::uint64_t, std::uint32_t> byHash;
};

// One instanced draw: mesh, drawn instanceCount times, as instances firstInstance onwards, and how its draws are drawn:
// cut-outs by pipelines that throw away their holes, double-sided ones without culling back faces.
struct DrawBatch {
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
    bool cutOut = false;
    bool doubleSided = false;
};

// The chosen draws grouped by mesh: order lists the draw indices in instance order, and each batch covers a run of them.
struct BatchedDraws {
    std::vector<std::size_t> order;
    std::vector<DrawBatch> batches;
};

/*
* Group the chosen draws (indices into draws) by how they are drawn (plain, double-sided, cut-out, double-sided cut-out)
* and then by mesh, meshes in handle order and each mesh's draws in the order they were chosen, so the same frame always gives the same instances. Instances are numbered from firstInstance, for views that
* write theirs after another view's in the same buffer (shadow tiles).
*/
BatchedDraws batchByMesh(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, std::uint32_t firstInstance = 0);
