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

    // Each triangle gets three separate GPU vertices.
    // This lets neighbouring faces have different normals.
    const std::size_t maxBytes =
        std::numeric_limits<Uint32>::max();

    const std::size_t bytesPerTriangle =
        3 * (sizeof(GpuVertex) + sizeof(Uint32));

    if (mesh.triangles.size() > maxBytes / bytesPerTriangle) {
        throw std::overflow_error("Combined mesh upload is too large");
    }

    // Validate positions before calculating normals.
    for (const Vec4& position : mesh.vertices) {
        if (!std::isfinite(position.x) ||
            !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            position.w != 1.0f) {
            throw std::invalid_argument(
                "GpuMesh requires finite XYZ positions with w = 1"
            );
        }
    }

    const std::size_t vertexCount = mesh.triangles.size() * 3;

    std::vector<GpuVertex> vertices;
    vertices.reserve(vertexCount);

    std::vector<Uint32> indices;
    indices.reserve(vertexCount);

    for (const Triangle& triangle : mesh.triangles) {
        if (triangle.first >= mesh.vertices.size() ||
            triangle.second >= mesh.vertices.size() ||
            triangle.third >= mesh.vertices.size()) {
            throw std::out_of_range(
                "Mesh triangle references a missing vertex"
            );
        }

        const Vec4& first = mesh.vertices[triangle.first];
        const Vec4& second = mesh.vertices[triangle.second];
        const Vec4& third = mesh.vertices[triangle.third];

        // Use double for the intermediate normal calculation.
        const double edgeAX = static_cast<double>(second.x) - first.x;
        const double edgeAY = static_cast<double>(second.y) - first.y;
        const double edgeAZ = static_cast<double>(second.z) - first.z;

        const double edgeBX = static_cast<double>(third.x) - first.x;
        const double edgeBY = static_cast<double>(third.y) - first.y;
        const double edgeBZ = static_cast<double>(third.z) - first.z;

        // Cross product: a direction perpendicular to the triangle.
        const double normalX = edgeAY * edgeBZ - edgeAZ * edgeBY;
        const double normalY = edgeAZ * edgeBX - edgeAX * edgeBZ;
        const double normalZ = edgeAX * edgeBY - edgeAY * edgeBX;

        const double normalLength =
            std::hypot(normalX, normalY, normalZ);

        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;

        // Degenerate triangles keep a zero normal.
        if (normalLength > 0.0) {
            nx = static_cast<float>(normalX / normalLength);
            ny = static_cast<float>(normalY / normalLength);
            nz = static_cast<float>(normalZ / normalLength);
        }

        const Uint32 firstIndex = static_cast<Uint32>(vertices.size());

        const Vec3 faceNormal{nx, ny, nz};

        const Vec3 firstNormal =
            triangle.normals[0].value_or(faceNormal);

        const Vec3 secondNormal =
            triangle.normals[1].value_or(faceNormal);

        const Vec3 thirdNormal =
            triangle.normals[2].value_or(faceNormal);

        vertices.push_back(GpuVertex{
            first.x, first.y, first.z,
            firstNormal.x, firstNormal.y, firstNormal.z
        });

        vertices.push_back(GpuVertex{
            second.x, second.y, second.z,
            secondNormal.x, secondNormal.y, secondNormal.z
        });

        vertices.push_back(GpuVertex{
            third.x, third.y, third.z,
            thirdNormal.x, thirdNormal.y, thirdNormal.z
        });

        indices.push_back(firstIndex);
        indices.push_back(firstIndex + 1);
        indices.push_back(firstIndex + 2);
    }

    const Uint32 vertexBytes = static_cast<Uint32>(
        vertices.size() * sizeof(GpuVertex)
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