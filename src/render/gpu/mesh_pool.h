#pragma once

#include "render/gpu/gpu_scene.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* The shared mesh buffer (docs/research/MESHPOOL.md, GPU-driven drawing's stage 6): every mesh that does not deform in
* one vertex pool and one index pool, each mesh a run of each. Its indices stay its own (counted from its first vertex),
* so a draw names the mesh by its first index and its first vertex (vertexOffset), and draws of different meshes can go
* in one multi-draw. Meshes are only added, between frames, so the pools only grow. Render-only and API-free: the
* Vulkan side (render/vulkan/vulkan_mesh_pool.h) keeps the buffers.
*/

// Where a mesh's vertices and indices are. pooled: in the pools; else its own buffers (a deforming mesh), from 0.
struct MeshPlacement {
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    bool pooled = false;
};

// The pools' smallest sizes, in elements: about 48 MB of vertices and 16 MB of indices.
inline constexpr std::uint64_t smallestPoolVertices = 1ull << 20;
inline constexpr std::uint64_t smallestPoolIndices = 1ull << 22;

// A pool's largest size, in bytes: under 4 GB, which some drivers' buffer limits are (MESHPOOL.md).
inline constexpr std::uint64_t largestPoolBytes = (1ull << 32) - 1;

/*
* The capacity (elements) a pool holding current needs to hold needed: current when it fits, else doubled until it
* does, at least smallest. Throws std::length_error past largestPoolBytes at elementBytes each.
*/
std::uint64_t poolCapacity(std::uint64_t current, std::uint64_t needed, std::uint64_t smallest, std::uint64_t elementBytes);

// The pools' running ends: each mesh added goes after the last.
class MeshPoolLayout {
public:
    // The mesh's place: its vertices and indices after everything placed before.
    MeshPlacement place(std::uint32_t vertexCount, std::uint32_t indexCount);

    std::uint64_t getVertices() const;
    std::uint64_t getIndices() const;

private:
    std::uint64_t vertices = 0;
    std::uint64_t indices = 0;
};

// One batch's draw command: its mesh's indices from where they are, its instances from the batch's first entry.
GpuDrawCommand packDrawCommand(const DrawBatch& batch, const MeshPlacement& placement);

/*
* Batches drawn by one call: a run of consecutive pooled batches drawn the same way (a multi-draw from the pools), or
* one batch of a mesh with its own buffers.
*/
struct DrawRun {
    std::size_t first = 0;
    std::size_t count = 0;
    bool pooled = false;
};

std::vector<DrawRun> drawRuns(const std::vector<DrawBatch>& batches, const std::function<bool(MeshHandle)>& pooled);
