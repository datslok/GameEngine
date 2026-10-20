#pragma once

#include "math/mat4.h"
#include "math/ray.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <optional>
#include <span>

/*
* Shared by the GPU tests that check ray queries: the CPU's answer to a ray, found with the physics raycast over every
* triangle of every instance the ray's mask lets it see, as the GPU's acceleration structures should find it.
*/

// One instance of a test scene: its mesh's triangles, where, and the draw flags its mask comes from (rayMaskFor).
struct CpuTraceInstance {
    const IndexedMesh* mesh = nullptr;
    Mat4 model = Mat4::identity();
    bool castsShadows = true;
    bool isStatic = false;
};

// What a ray hit first.
struct CpuTraceHit {
    float distance = 0.0f;
    std::uint32_t instance = 0;
    std::uint32_t triangle = 0;
    bool front = false;
    Vec3 normal;         // the mesh's own normal, interpolated at the hit (not normalised)
    float cosine = 1.0f; // between the ray and the triangle's normal, unsigned: small for a grazing hit
};

// Corner number corner (counted through the index list) of the mesh, placed by model.
Vec3 cornerPosition(const IndexedMesh& mesh, std::uint32_t corner, const Mat4& model);

std::optional<CpuTraceHit> traceOnCpu(std::span<const CpuTraceInstance> instances, const Ray& ray, float maxDistance, std::uint32_t mask);
