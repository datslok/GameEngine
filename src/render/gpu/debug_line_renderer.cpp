#include "render/gpu/debug_line_renderer.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/gpu_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

DebugLineRenderer::DebugLineRenderer(SDL_GPUDevice* device, SDL_GPUTextureFormat colourFormat):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("DebugLineRenderer requires a GPU device");
    }

    try {
        createPipeline(colourFormat);
    }
    catch (...) {
        cleanup();
        throw;
    }
}

DebugLineRenderer::~DebugLineRenderer() {
    cleanup();
}

/*
* Debug lines are drawn as a line list (every two vertices one line), unlit, into the same colour and depth targets as the
* scene but ignoring depth, so they show through walls: they are for seeing what is hidden.
*/
void DebugLineRenderer::createPipeline(SDL_GPUTextureFormat colourFormat) {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/debug_line.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        fragmentShader = loadShader(device, "assets/shaders/debug_line.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = colourFormat;

        SDL_GPUVertexBufferDescription vertexDescription{};
        vertexDescription.slot = 0;
        vertexDescription.pitch = static_cast<Uint32>(sizeof(DebugLineVertex));
        vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attributes[2]{};
        attributes[0].location = 0;
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset = static_cast<Uint32>(offsetof(DebugLineVertex, position));
        attributes[1].location = 1;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].offset = static_cast<Uint32>(offsetof(DebugLineVertex, colour));

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader;
        info.fragment_shader = fragmentShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &vertexDescription;
        info.vertex_input_state.num_vertex_attributes = 2;
        info.vertex_input_state.vertex_attributes = attributes;

        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &colourTarget;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.depth_stencil_state.enable_depth_test = false;
        info.depth_stencil_state.enable_depth_write = false;

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (pipeline == nullptr) {
            throw gpuError("Debug line pipeline creation failed");
        }
    }
    catch (...) {
        if (fragmentShader != nullptr) {
            SDL_ReleaseGPUShader(device, fragmentShader);
        }

        if (vertexShader != nullptr) {
            SDL_ReleaseGPUShader(device, vertexShader);
        }

        throw;
    }

    SDL_ReleaseGPUShader(device, fragmentShader);
    SDL_ReleaseGPUShader(device, vertexShader);
}

/*
* The line count changes every frame, so the buffer is reused and only grows, doubling so a slowly growing count does not
* reallocate every frame. The old buffers are released only once both new ones exist, so a failure leaves the old ones usable.
*/
void DebugLineRenderer::ensureCapacity(Uint32 vertexCount) {
    if (vertexCount <= capacity) {
        return;
    }

    const Uint32 newCapacity = std::max({vertexCount, capacity * 2, Uint32{1024}});

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bufferInfo.size = newCapacity * static_cast<Uint32>(sizeof(DebugLineVertex));

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bufferInfo.size;

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &bufferInfo);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    if (buffer == nullptr || transfer == nullptr) {
        const std::runtime_error error = gpuError("Debug line buffer creation failed");

        if (buffer != nullptr) {
            SDL_ReleaseGPUBuffer(device, buffer);
        }

        if (transfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }

        throw error;
    }

    if (vertexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(device, vertexBuffer);
    }

    if (transferBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    vertexBuffer = buffer;
    transferBuffer = transfer;
    capacity = newCapacity;
}

/*
* World and screen lines share one buffer, world lines first, so a single copy uploads both.
* Cycling lets the GPU keep reading last frame's copy while this frame's is written.
*/
void DebugLineRenderer::upload(SDL_GPUCommandBuffer* commands, const std::vector<DebugLine>& worldLines, const std::vector<DebugLine>& screenLines) {
    std::vector<DebugLineVertex> vertices = buildDebugLineVertices(worldLines);
    const std::vector<DebugLineVertex> screenVertices = buildDebugLineVertices(screenLines);

    worldVertexCount = static_cast<Uint32>(vertices.size());
    screenVertexCount = static_cast<Uint32>(screenVertices.size());
    vertices.insert(vertices.end(), screenVertices.begin(), screenVertices.end());

    if (vertices.empty()) {
        return;
    }

    const Uint32 vertexCount = static_cast<Uint32>(vertices.size());

    try {
        ensureCapacity(vertexCount);
    }
    catch (...) {
        // Nothing usable was uploaded, so draw() must not draw last frame's leftovers.
        worldVertexCount = 0;
        screenVertexCount = 0;
        throw;
    }

    const Uint32 byteCount = vertexCount * static_cast<Uint32>(sizeof(DebugLineVertex));
    void* destination = SDL_MapGPUTransferBuffer(device, transferBuffer, true);

    if (destination == nullptr) {
        worldVertexCount = 0;
        screenVertexCount = 0;
        throw gpuError("Debug line transfer buffer mapping failed");
    }

    std::memcpy(destination, vertices.data(), byteCount);
    SDL_UnmapGPUTransferBuffer(device, transferBuffer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commands);

    if (copyPass == nullptr) {
        worldVertexCount = 0;
        screenVertexCount = 0;
        throw gpuError("Debug line copy pass creation failed");
    }

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transferBuffer;

    SDL_GPUBufferRegion target{};
    target.buffer = vertexBuffer;
    target.size = byteCount;

    SDL_UploadToGPUBuffer(copyPass, &source, &target, true);
    SDL_EndGPUCopyPass(copyPass);
}

/*
* World lines go through the camera; screen lines are already in window pixels, so a flat projection maps (0, 0) to the
* top-left corner and (width, height) to the bottom-right.
*/
void DebugLineRenderer::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection, Uint32 frameWidth, Uint32 frameHeight) {
    if (worldVertexCount > 0) {
        drawRange(commands, pass, viewProjection, 0, worldVertexCount);
    }

    if (screenVertexCount > 0) {
        const Mat4 pixels = toGpuDepthRange(Mat4::orthographic(0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, -1.0f, 1.0f));
        drawRange(commands, pass, pixels, worldVertexCount, screenVertexCount);
    }
}

void DebugLineRenderer::drawRange(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& transform, Uint32 firstVertex, Uint32 vertexCount) {
    SDL_BindGPUGraphicsPipeline(pass, pipeline);

    SDL_GPUBufferBinding binding{};
    binding.buffer = vertexBuffer;
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

    float matrix[16]{};
    writeColumnMajor(transform, matrix);
    SDL_PushGPUVertexUniformData(commands, 0, matrix, static_cast<Uint32>(sizeof(matrix)));

    SDL_DrawGPUPrimitives(pass, vertexCount, 1, firstVertex, 0);
}

void DebugLineRenderer::cleanup() noexcept {
    if (device == nullptr) {
        return;
    }

    if (pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        pipeline = nullptr;
    }

    if (vertexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(device, vertexBuffer);
        vertexBuffer = nullptr;
    }

    if (transferBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
        transferBuffer = nullptr;
    }

    capacity = 0;
}
