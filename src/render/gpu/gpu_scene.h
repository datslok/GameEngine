#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/material.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

/*
* The GPU scene: the frame's objects, materials and lights as arrays in storage buffers, written once per frame, which
* the shaders index themselves (an object by its instance number, its material by the number the object carries, its
* textures by the numbers the material carries). A draw then names only a mesh and a run of instances, so every object
* with the same mesh is one instanced draw, and nothing is bound per object. These are the parts that do not touch the
* graphics API: the records' layouts (std430, mirrored in the shaders) and how the frame's draws are grouped.
*/

/*
* One object as triangle.vert reads it from the instance buffer: what each draw used to push as its vertex uniform block
* (the camera-relative transform and model matrix, the normal matrix and the handedness, all column-major), and the
* number of its material in the frame's material table.
*/
struct GpuInstance {
    float transform[16];    // the camera-relative view-projection times the camera-relative model matrix
    float model[16];        // places the vertex relative to the camera
    float normalMatrix[16]; // inverse transpose of the model's 3x3 part (zero if squashed flat)
    float handedness[4];    // x: -1 if the model matrix mirrors space, else +1
    std::uint32_t material[4]; // x: the material's number in the frame's material buffer
};

static_assert(sizeof(GpuInstance) == 224, "GpuInstance must match the shader's InstanceData struct");

// One shadow caster in one shadow tile, as shadow.vert reads it: the tile's view-projection times the model matrix.
struct GpuShadowInstance {
    float transform[16];
};

static_assert(sizeof(GpuShadowInstance) == 64, "GpuShadowInstance must match the shader's ShadowInstanceData struct");

/*
* The frame's small uniform block, the same for every pixel (std140): the ambient light, how many lights of each kind
* the light buffers hold this frame, and the size of one shadow atlas texel.
*/
struct GpuFrameData {
    float ambient[4];
    std::int32_t counts[4]; // x: directional lights, y: point lights, z: spotlights
    float atlasTexel[4];    // xy: one texel's size in atlas coordinates
};

static_assert(sizeof(GpuFrameData) == 48, "GpuFrameData must match the shader's FrameData block");

// The instance record for one draw, worked out exactly as the per-draw uniforms were (so the picture stays the same).
GpuInstance packInstance(const DrawItem& draw, const Mat4& relativeViewProjection, const Vec3& cameraOrigin, std::uint32_t material);

GpuShadowInstance packShadowInstance(const Mat4& tileMatrix, const Mat4& model);

GpuFrameData makeFrameData(const GpuLights& lights);

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

// One instanced draw: mesh, drawn instanceCount times, as instances firstInstance onwards.
struct DrawBatch {
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
};

// The chosen draws grouped by mesh: order lists the draw indices in instance order, and each batch covers a run of them.
struct BatchedDraws {
    std::vector<std::size_t> order;
    std::vector<DrawBatch> batches;
};

/*
* Group the chosen draws (indices into draws) by mesh, meshes in handle order and each mesh's draws in the order they were
* chosen, so the same frame always gives the same instances. Instances are numbered from firstInstance, for views that
* write theirs after another view's in the same buffer (shadow tiles).
*/
BatchedDraws batchByMesh(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, std::uint32_t firstInstance = 0);
