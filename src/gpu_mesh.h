#pragma once

#include "mesh.h"

#include <SDL3/SDL.h>

// Layout shared by mesh uploads and the graphics pipeline.
struct GpuVertex {
    float x;
    float y;
    float z;

    float nx;
    float ny;
    float nz;

    float u;
    float v;
};

class GpuMesh {
public:
    // Upload the mesh once during construction.
    GpuMesh(SDL_GPUDevice* device, const Mesh& mesh);
    ~GpuMesh();

    // GPU buffers must have a single owner.
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    SDL_GPUBuffer* getVertexBuffer() const;
    SDL_GPUBuffer* getIndexBuffer() const;
    Uint32 getIndexCount() const;

private:
    // Borrowed device: it must outlive this GpuMesh.
    SDL_GPUDevice* device = nullptr;

    // Buffers owned by this object.
    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUBuffer* indexBuffer = nullptr;

    Uint32 indexCount = 0;

    void cleanup() noexcept;
};