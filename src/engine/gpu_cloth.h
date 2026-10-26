#pragma once

#include "cloth/cloth.h"
#include "ecs/world.h"
#include "math/aabb.h"
#include "render/gpu/cloth_gpu.h"
#include "render/gpu/gpu_timing.h"
#include "scene/deforming_mesh.h"

#include <cstdint>
#include <span>
#include <vector>

/*
* The engine's side of cloth on the GPU: once a frame, the cloths a game stepped on the GPU (Cloth::onGpu) are handed
* to the renderer with the ticks they queued, and the points the renderer read back a few frames later are handed to
* the cloths, so picking, grabbing and the debug lines keep working on them. Their meshes are shaped on the GPU, so
* the frame lists them with bounds grown by how far their points can have moved since.
*/

/*
* The GPU's time for one tick of every cloth on it, for the tick budget (fitsTickBudget): a measured frame's cloth time
* over the ticks it ran (RenderStats::clothTicks). A frame not measured, or one that ran no ticks, says nothing about a
* tick's cost, so the estimate stays. The cloth's time includes its once-a-frame work (shaping the meshes, the copy
* back), so with few ticks a frame the estimate errs high, and the budget towards dropping a tick.
*/
double gpuClothTickSeconds(double estimate, const GpuTimings& timings, std::uint32_t ticks);

// A cloth's key for the renderer: its entity's slot and generation, so a recycled slot is a new cloth.
std::uint64_t clothKey(Entity entity);

// Every cloth on the GPU with the ticks it queued (moved out, so none are run twice) and its DeformingMesh's mesh.
std::vector<ClothWork> takeClothWork(World& world);

// The GPU's late points into their cloths (positions and velocities) and meshes (both of their ticks, so they are not
// blended). A readback for a cloth that is gone, back on the CPU, or whose size no longer matches, is ignored.
void applyClothReadbacks(World& world, std::span<const ClothReadback> readbacks);

// The box round the points grown by how far the fastest can go in lateSeconds, and by thickness.
Aabb lateClothBounds(const ClothParticles& particles, float lateSeconds, float thickness);

/*
* A GPU cloth's entry in the frame: shaped on the GPU, its bounds lateClothBounds, its shape fingerprint changing every
* frame (frameNumber), since the CPU does not know whether the GPU's points moved: its shadows are drawn again every frame.
*/
DeformedMesh gpuClothDeformedMesh(const Cloth& cloth, MeshHandle mesh, float lateSeconds, std::uint64_t frameNumber);
