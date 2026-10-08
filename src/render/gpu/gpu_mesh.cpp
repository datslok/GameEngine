#include "render/gpu/gpu_mesh.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    std::runtime_error meshGpuError(const char* message) {
        return std::runtime_error(
            std::string{message} + ": " + SDL_GetError()
        );
    }
}

/*
* The mesh is already in its final indexed form (see buildIndexedMesh), so this only checks it and copies it to GPU memory.
*/
GpuMesh::GpuMesh(SDL_GPUDevice* device, const IndexedMesh& mesh):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("GpuMesh requires a GPU device");
    }

    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        throw std::invalid_argument(
            "GpuMesh requires vertices and whole triangles of indices"
        );
    }

    // Upload sizes are 32-bit.
    const std::size_t maxBytes = std::numeric_limits<Uint32>::max();

    if (mesh.vertices.size() > maxBytes / sizeof(MeshVertex) ||
        mesh.indices.size() > maxBytes / sizeof(Uint32) ||
        mesh.vertices.size() * sizeof(MeshVertex) > maxBytes - mesh.indices.size() * sizeof(Uint32)) {
        throw std::overflow_error("Combined mesh upload is too large");
    }

    // An out-of-range index would make the GPU read past the vertex buffer.
    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::out_of_range("Mesh index references a missing vertex");
        }
    }

    const std::vector<MeshVertex>& vertices = mesh.vertices;
    const std::vector<std::uint32_t>& indices = mesh.indices;

    const Uint32 vertexBytes = static_cast<Uint32>(
        vertices.size() * sizeof(MeshVertex)
    );

    const Uint32 indexBytes = static_cast<Uint32>(
        indices.size() * sizeof(Uint32)
    );

    indexCount = static_cast<Uint32>(indices.size());

    SDL_GPUTransferBuffer* transfer = nullptr;
    SDL_GPUCommandBuffer* commands = nullptr;

    try {
        SDL_GPUBufferCreateInfo vertexInfo{};
        vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vertexInfo.size = vertexBytes;

        vertexBuffer = SDL_CreateGPUBuffer(device, &vertexInfo);

        if (vertexBuffer == nullptr) {
            throw meshGpuError("Vertex buffer creation failed");
        }

        SDL_GPUBufferCreateInfo indexInfo{};
        indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        indexInfo.size = indexBytes;

        indexBuffer = SDL_CreateGPUBuffer(device, &indexInfo);

        if (indexBuffer == nullptr) {
            throw meshGpuError("Index buffer creation failed");
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = vertexBytes + indexBytes;

        transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

        if (transfer == nullptr) {
            throw meshGpuError("Transfer buffer creation failed");
        }

        void* mapped =
            SDL_MapGPUTransferBuffer(device, transfer, false);

        if (mapped == nullptr) {
            throw meshGpuError("Transfer buffer mapping failed");
        }

        Uint8* destination = static_cast<Uint8*>(mapped);

        std::memcpy(destination, vertices.data(), vertexBytes);
        std::memcpy(
            destination + vertexBytes,
            indices.data(),
            indexBytes
        );

        SDL_UnmapGPUTransferBuffer(device, transfer);

        commands = SDL_AcquireGPUCommandBuffer(device);

        if (commands == nullptr) {
            throw meshGpuError("Upload command buffer acquisition failed");
        }

        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commands);

        if (copyPass == nullptr) {
            throw meshGpuError("GPU copy pass creation failed");
        }

        SDL_GPUTransferBufferLocation vertexSource{};
        vertexSource.transfer_buffer = transfer;

        SDL_GPUBufferRegion vertexRegion{};
        vertexRegion.buffer = vertexBuffer;
        vertexRegion.size = vertexBytes;

        SDL_UploadToGPUBuffer(
            copyPass,
            &vertexSource,
            &vertexRegion,
            false
        );

        SDL_GPUTransferBufferLocation indexSource{};
        indexSource.transfer_buffer = transfer;
        indexSource.offset = vertexBytes;

        SDL_GPUBufferRegion indexRegion{};
        indexRegion.buffer = indexBuffer;
        indexRegion.size = indexBytes;

        SDL_UploadToGPUBuffer(
            copyPass,
            &indexSource,
            &indexRegion,
            false
        );

        SDL_EndGPUCopyPass(copyPass);

        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        commands = nullptr;

        if (!submitted) {
            throw meshGpuError("Mesh upload submission failed");
        }
    }
    catch (...) {
        if (commands != nullptr) {
            SDL_CancelGPUCommandBuffer(commands);
        }

        if (transfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }

        cleanup();
        throw;
    }

    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

GpuMesh::~GpuMesh() {
    cleanup();
}

void GpuMesh::cleanup() noexcept {
    if (indexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(device, indexBuffer);
        indexBuffer = nullptr;
    }

    if (vertexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(device, vertexBuffer);
        vertexBuffer = nullptr;
    }

    indexCount = 0;
}

SDL_GPUBuffer* GpuMesh::getVertexBuffer() const {
    return vertexBuffer;
}

SDL_GPUBuffer* GpuMesh::getIndexBuffer() const {
    return indexBuffer;
}

Uint32 GpuMesh::getIndexCount() const {
    return indexCount;
}