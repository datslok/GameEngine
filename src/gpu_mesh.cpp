#include "gpu_mesh.h"

#include <algorithm>
#include <cmath>
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

GpuMesh::GpuMesh(SDL_GPUDevice* device, const Mesh& mesh):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("GpuMesh requires a GPU device");
    }

    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument(
            "GpuMesh requires vertices and triangles"
        );
    }

    // SDL buffer sizes are measured in bytes using Uint32.
    const std::size_t maxBytes =
        std::numeric_limits<Uint32>::max();

    if (mesh.vertices.size() > maxBytes / sizeof(GpuVertex)) {
        throw std::overflow_error("Mesh vertex data is too large");
    }

    if (mesh.triangles.size() > maxBytes / (3 * sizeof(Uint32))) {
        throw std::overflow_error("Mesh index data is too large");
    }

    const Uint32 vertexBytes = static_cast<Uint32>(
        mesh.vertices.size() * sizeof(GpuVertex)
    );

    const Uint32 indexBytes = static_cast<Uint32>(
        mesh.triangles.size() * 3 * sizeof(Uint32)
    );

    if (vertexBytes > std::numeric_limits<Uint32>::max() - indexBytes) {
        throw std::overflow_error("Combined mesh upload is too large");
    }

    std::vector<GpuVertex> vertices;
    vertices.reserve(mesh.vertices.size());

    for (const Vec4& position : mesh.vertices) {
        if (!std::isfinite(position.x) ||
            !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            position.w != 1.0f) {
            throw std::invalid_argument(
                "GpuMesh requires finite XYZ positions with w = 1"
            );
        }

        // Temporary position-based colours for the GPU demo.
        vertices.push_back(GpuVertex{
            position.x,
            position.y,
            position.z,
            std::clamp(position.x * 0.5f + 0.5f, 0.0f, 1.0f),
            std::clamp(position.y * 0.5f + 0.5f, 0.0f, 1.0f),
            std::clamp(position.z * 0.5f + 0.5f, 0.0f, 1.0f)
        });
    }

    std::vector<Uint32> indices;
    indices.reserve(mesh.triangles.size() * 3);

    for (const Triangle& triangle : mesh.triangles) {
        if (triangle.first >= mesh.vertices.size() ||
            triangle.second >= mesh.vertices.size() ||
            triangle.third >= mesh.vertices.size()) {
            throw std::out_of_range(
                "Mesh triangle references a missing vertex"
            );
        }

        indices.push_back(static_cast<Uint32>(triangle.first));
        indices.push_back(static_cast<Uint32>(triangle.second));
        indices.push_back(static_cast<Uint32>(triangle.third));
    }

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