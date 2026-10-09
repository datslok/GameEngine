#include "render/gpu/gpu_renderer.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"

#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {
    // Shaders expect column-major matrices; Mat4 is row-major.
    void writeColumnMajor(const Mat4& matrix, float* target) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                target[column * 4 + row] = matrix.values[row][column];
            }
        }
    }

    std::runtime_error gpuError(const char* message) {
        return std::runtime_error(
            std::string{message} + ": " + SDL_GetError()
        );
    }
    SDL_GPUShader* loadShader(
        SDL_GPUDevice* device,
        const char* filename,
        SDL_GPUShaderStage stage,
        Uint32 uniformBufferCount,
        Uint32 samplerCount
    ) {
        std::size_t codeSize = 0;
        void* code = SDL_LoadFile(filename, &codeSize);

        if (code == nullptr) {
            throw gpuError(filename);
        }

        SDL_GPUShaderCreateInfo info{};
        info.code = static_cast<const Uint8*>(code);
        info.code_size = codeSize;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.stage = stage;
        info.num_uniform_buffers = uniformBufferCount;
        info.num_samplers = samplerCount;

        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);

        if (shader == nullptr) {
            const std::runtime_error error =
                gpuError("Shader creation failed");

            SDL_free(code);
            throw error;
        }

        SDL_free(code);
        return shader;
    }
}

GpuRenderer::GpuRenderer(SDL_Window* window):
    window(window)
{
    if (window == nullptr) {
        throw std::invalid_argument("GpuRenderer requires a window");
    }

    try {
        // Select Vulkan and the shader format we will use later.
        device = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV,
            true,
            "vulkan"
        );

        if (device == nullptr) {
            throw gpuError("GPU device creation failed");
        }

        // Connect this window to the GPU device.
        if (!SDL_ClaimWindowForGPUDevice(device, window)) {
            throw gpuError("GPU window claim failed");
        }

        windowClaimed = true;

        // SDL lets the CPU queue two frames ahead of the GPU by default. Each queued frame was built from older input, so allow only one.
        if (!SDL_SetGPUAllowedFramesInFlight(device, 1)) {
            throw gpuError("Could not limit frames in flight");
        }

        // Before the pipeline, which is built for the swapchain's format.
        useLinearSwapchain();

        createPipeline();
        createShadowResources();
        createDebugLinePipeline();
        createWhiteTexture();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

/*
* The shader does its lighting in linear light. An sRGB swapchain (SDR_LINEAR) encodes each pixel as it is written,
* so the screen gets the gamma-encoded values it expects, and blending happens in linear light too.
* Without one, linear values would be shown as they are and mid-tones would look far too dark, so warn loudly.
*/
void GpuRenderer::useLinearSwapchain() {
    if (!SDL_WindowSupportsGPUSwapchainComposition(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
        SDL_Log("sRGB swapchain not supported: colours will look too dark");
        return;
    }

    if (!SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR, SDL_GPU_PRESENTMODE_VSYNC)) {
        throw gpuError("Could not switch to an sRGB swapchain");
    }

    swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
}

void GpuRenderer::createPipeline() {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(
            device,
            "assets/shaders/triangle.vert.spv",
            SDL_GPU_SHADERSTAGE_VERTEX,
            1,
            0
        );

        fragmentShader = loadShader(
            device,
            "assets/shaders/triangle.frag.spv",
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            3, // Material colour, lights, shadow data.
            2  // Colour texture, shadow atlas.
        );

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format =
            SDL_GetGPUSwapchainTextureFormat(device, window);


        SDL_GPUVertexBufferDescription vertexDescription{};
        vertexDescription.slot = 0;
        vertexDescription.pitch = static_cast<Uint32>(sizeof(MeshVertex));
        vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attributes[3]{};

        // Position: shader input location 0.
        attributes[0].location = 0;
        attributes[0].buffer_slot = 0;
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset = static_cast<Uint32>(offsetof(MeshVertex, position));

        // Surface normal.
        attributes[1].location = 1;
        attributes[1].buffer_slot = 0;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].offset = static_cast<Uint32>(offsetof(MeshVertex, normal));

        // Texture coordinates: shader input location 2.
        attributes[2].location = 2;
        attributes[2].buffer_slot = 0;
        attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[2].offset = static_cast<Uint32>(offsetof(MeshVertex, uv));

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader;
        info.fragment_shader = fragmentShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

        // Skip triangles facing away from the camera.
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;

        // Counter-clockwise vertex order identifies a front face.
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &colourTarget;

        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &vertexDescription;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.vertex_input_state.vertex_attributes = attributes;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (pipeline == nullptr) {
            throw gpuError("Graphics pipeline creation failed");
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

    // The pipeline retains what it needs from the shaders.
    SDL_ReleaseGPUShader(device, fragmentShader);
    SDL_ReleaseGPUShader(device, vertexShader);
}

/*
* The atlas holds every shadow view, one per tile (8 across, 4 down). It is both drawn into (as a depth
* target) and read (as a texture). The sampler compares instead of returning depth: linear filtering then blends the
* four nearest comparisons, which already softens edges a little before the shader's 3x3 average.
* The pipeline draws depth only, from positions alone. Both sides of triangles are drawn, so open meshes (a plane) still
* cast shadows, and a small slope-scaled bias pushes depths away from the light on surfaces seen at a grazing angle,
* where shadow acne is worst.
*/
void GpuRenderer::createShadowResources() {
    const SDL_GPUTextureUsageFlags atlasUsage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, atlasUsage)) {
        throw std::runtime_error("This GPU cannot sample a 32-bit depth texture, which shadows need");
    }

    SDL_GPUTextureCreateInfo atlasInfo{};
    atlasInfo.type = SDL_GPU_TEXTURETYPE_2D;
    atlasInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    atlasInfo.usage = atlasUsage;
    atlasInfo.width = shadowTileSize * shadowAtlasColumns;
    atlasInfo.height = shadowTileSize * shadowAtlasRows;
    atlasInfo.layer_count_or_depth = 1;
    atlasInfo.num_levels = 1;
    atlasInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

    shadowAtlas = SDL_CreateGPUTexture(device, &atlasInfo);

    if (shadowAtlas == nullptr) {
        throw gpuError("Shadow atlas creation failed");
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    // Lit when our depth is no further from the light than the nearest surface it saw.
    samplerInfo.enable_compare = true;
    samplerInfo.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

    shadowSampler = SDL_CreateGPUSampler(device, &samplerInfo);

    if (shadowSampler == nullptr) {
        throw gpuError("Shadow sampler creation failed");
    }

    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/shadow.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        fragmentShader = loadShader(device, "assets/shaders/shadow.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);

        SDL_GPUVertexBufferDescription vertexDescription{};
        vertexDescription.slot = 0;
        vertexDescription.pitch = static_cast<Uint32>(sizeof(MeshVertex));
        vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute position{};
        position.location = 0;
        position.buffer_slot = 0;
        position.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        position.offset = static_cast<Uint32>(offsetof(MeshVertex, position));

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader;
        info.fragment_shader = fragmentShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 1.0f;
        info.rasterizer_state.depth_bias_slope_factor = 1.5f;

        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &vertexDescription;
        info.vertex_input_state.num_vertex_attributes = 1;
        info.vertex_input_state.vertex_attributes = &position;

        info.target_info.num_color_targets = 0;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

        shadowPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (shadowPipeline == nullptr) {
            throw gpuError("Shadow pipeline creation failed");
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
* Debug lines are drawn as a line list (every two vertices one line), unlit, into the same colour and depth targets as the
* scene but ignoring depth, so they show through walls: they are for seeing what is hidden.
*/
void GpuRenderer::createDebugLinePipeline() {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/debug_line.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        fragmentShader = loadShader(device, "assets/shaders/debug_line.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

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

        debugLinePipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (debugLinePipeline == nullptr) {
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
* reallocate every frame. Cycling lets the GPU keep reading last frame's copy while this frame's is written.
*/
void GpuRenderer::uploadDebugLines(const std::vector<DebugLineVertex>& vertices) {
    const Uint32 vertexCount = static_cast<Uint32>(vertices.size());

    if (vertexCount > debugLineCapacity) {
        const Uint32 capacity = std::max({vertexCount, debugLineCapacity * 2, Uint32{1024}});

        SDL_GPUBufferCreateInfo bufferInfo{};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size = capacity * static_cast<Uint32>(sizeof(DebugLineVertex));

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

        if (debugLineBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(device, debugLineBuffer);
        }

        if (debugLineTransfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, debugLineTransfer);
        }

        debugLineBuffer = buffer;
        debugLineTransfer = transfer;
        debugLineCapacity = capacity;
    }

    const Uint32 byteCount = vertexCount * static_cast<Uint32>(sizeof(DebugLineVertex));
    void* destination = SDL_MapGPUTransferBuffer(device, debugLineTransfer, true);

    if (destination == nullptr) {
        throw gpuError("Debug line transfer buffer mapping failed");
    }

    std::memcpy(destination, vertices.data(), byteCount);
    SDL_UnmapGPUTransferBuffer(device, debugLineTransfer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commands);

    if (copyPass == nullptr) {
        throw gpuError("Debug line copy pass creation failed");
    }

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = debugLineTransfer;

    SDL_GPUBufferRegion target{};
    target.buffer = debugLineBuffer;
    target.size = byteCount;

    SDL_UploadToGPUBuffer(copyPass, &source, &target, true);
    SDL_EndGPUCopyPass(copyPass);
}

void GpuRenderer::drawDebugLines(Uint32 vertexCount) {
    SDL_BindGPUGraphicsPipeline(pass, debugLinePipeline);

    SDL_GPUBufferBinding binding{};
    binding.buffer = debugLineBuffer;
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

    float matrix[16]{};
    writeColumnMajor(viewProjection, matrix);
    SDL_PushGPUVertexUniformData(commands, 0, matrix, static_cast<Uint32>(sizeof(matrix)));

    SDL_DrawGPUPrimitives(pass, vertexCount, 1, 0, 0);
}

GpuRenderer::~GpuRenderer() {
    cleanup();
}

void GpuRenderer::ensureDepthTexture(Uint32 width, Uint32 height) {
    if (depthTexture != nullptr &&
        depthWidth == width &&
        depthHeight == height) {
        return;
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device, &info);

    if (replacement == nullptr) {
        throw gpuError("Depth texture creation failed");
    }

    if (depthTexture != nullptr) {
        SDL_ReleaseGPUTexture(device, depthTexture);
    }

    depthTexture = replacement;
    depthWidth = width;
    depthHeight = height;
}

void GpuRenderer::cleanup() noexcept {
    if (device != nullptr) {
        // Finish any frame still open during shutdown.
        if (pass != nullptr) {
            SDL_EndGPURenderPass(pass);
            pass = nullptr;
        }

        if (commands != nullptr) {
            SDL_SubmitGPUCommandBuffer(commands);
            commands = nullptr;
        }
        // Finish outstanding work before releasing resources.
        SDL_WaitForGPUIdle(device);

        // Release all meshes and textures before destroying their GPU device.
        meshes.clear();
        textures.clear();
        whiteTexture.reset();

        if (depthTexture != nullptr) {
            SDL_ReleaseGPUTexture(device, depthTexture);
            depthTexture = nullptr;
            depthWidth = 0;
            depthHeight = 0;
        }

        if (pipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            pipeline = nullptr;
        }

        if (debugLinePipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, debugLinePipeline);
            debugLinePipeline = nullptr;
        }

        if (debugLineBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(device, debugLineBuffer);
            debugLineBuffer = nullptr;
        }

        if (debugLineTransfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, debugLineTransfer);
            debugLineTransfer = nullptr;
        }

        debugLineCapacity = 0;

        if (shadowPipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, shadowPipeline);
            shadowPipeline = nullptr;
        }

        if (shadowSampler != nullptr) {
            SDL_ReleaseGPUSampler(device, shadowSampler);
            shadowSampler = nullptr;
        }

        if (shadowAtlas != nullptr) {
            SDL_ReleaseGPUTexture(device, shadowAtlas);
            shadowAtlas = nullptr;
        }

        if (windowClaimed) {
            SDL_ReleaseWindowFromGPUDevice(device, window);
            windowClaimed = false;
        }

        SDL_DestroyGPUDevice(device);
        device = nullptr;
    }

    // The window belongs to its owner, so it is released from the device above but never destroyed here.
}

SDL_GPUDevice* GpuRenderer::getDevice() const {
    return device;
}


/*
* The whole frame arrives at once, so the renderer decides how to draw it: first the shadow-casting light's view of the
* scene into the shadow atlas, then the camera's view, which reads that atlas. Culling will later skip draws the camera cannot see.
* If anything fails part-way, the frame is still submitted (a swapchain image was acquired), then the error is passed on.
*/
bool GpuRenderer::render(const FrameDescription& frame) {
    SDL_GPUTexture* swapchainTexture = acquireFrame();

    if (swapchainTexture == nullptr) {
        return false;
    }

    try {
        // Only now is the frame's size known, so the camera's lens is fitted to it here.
        Camera camera = frame.camera;
        camera.setAspectRatio(getFrameAspectRatio());

        const LightUniformData lights = packLighting(frame.lighting, camera.getPosition());
        const ShadowPlan shadows = planShadows(frame.lighting, camera);

        // Copies must happen outside render passes, so the debug lines are uploaded first.
        const std::vector<DebugLineVertex> debugVertices = buildDebugLineVertices(frame.debugLines);

        if (!debugVertices.empty()) {
            uploadDebugLines(debugVertices);
        }

        if (!shadows.tileMatrices.empty()) {
            drawShadows(frame, shadows.tileMatrices);
        }

        beginMainPass(swapchainTexture);

        // The projection follows OpenGL's depth range; the GPU's differs, and that is the renderer's business, so it is converted here.
        // View and projection are combined once per frame, not once per object.
        viewProjection = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix();

        // Pushed uniform data stays in effect for every later draw, so the lights and shadow data are sent once per frame.
        SDL_PushGPUFragmentUniformData(commands, 1, &lights, static_cast<Uint32>(sizeof(lights)));
        SDL_PushGPUFragmentUniformData(commands, 2, &shadows.uniforms, static_cast<Uint32>(sizeof(shadows.uniforms)));

        for (const DrawItem& draw : frame.draws) {
            drawMesh(draw.mesh, draw.model, draw.material);
        }

        // Last, so they are drawn over the finished scene.
        if (!debugVertices.empty()) {
            drawDebugLines(static_cast<Uint32>(debugVertices.size()));
        }
    }
    catch (...) {
        abandonFrame();
        throw;
    }

    endFrame();
    return true;
}

/*
* Starts the frame's command buffer and gets the image that will become the next displayed frame.
* Returns null, with nothing left open, when there is no image (a minimized window).
*/
SDL_GPUTexture* GpuRenderer::acquireFrame() {
    if (commands != nullptr) {
        throw std::logic_error("Finish the current frame before beginning another");
    }

    commands = SDL_AcquireGPUCommandBuffer(device);

    if (commands == nullptr) {
        throw gpuError("GPU command buffer acquisition failed");
    }

    SDL_GPUTexture* swapchainTexture = nullptr;
    Uint32 frameWidth = 0;
    Uint32 frameHeight = 0;

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchainTexture, &frameWidth, &frameHeight)) {
        const std::runtime_error error = gpuError("Swapchain acquisition failed");

        SDL_CancelGPUCommandBuffer(commands);
        commands = nullptr;
        throw error;
    }

    // A minimized window may have no image available.
    if (swapchainTexture == nullptr) {
        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        commands = nullptr;

        if (!submitted) {
            throw gpuError("GPU submission failed");
        }

        SDL_Delay(10);
        return nullptr;
    }

    try {
        ensureDepthTexture(frameWidth, frameHeight);
    }
    catch (...) {
        abandonFrame();
        throw;
    }

    return swapchainTexture;
}

/*
* The shadow maps: the scene drawn once per shadow view (six for each point light, one for each spotlight or directional
* light), each into its own tile of the atlas (the viewport picks the tile). Only depth is kept, so each texel holds how
* far the nearest surface is from the light in that direction. Meshes that do not cast shadows are left out.
*/
void GpuRenderer::drawShadows(const FrameDescription& frame, const std::vector<Mat4>& tileMatrices) {
    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = shadowAtlas;
    depthTarget.clear_depth = 1.0f;
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_STORE; // The main pass reads it.
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle = true;

    pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depthTarget);

    if (pass == nullptr) {
        throw gpuError("Shadow pass creation failed");
    }

    SDL_BindGPUGraphicsPipeline(pass, shadowPipeline);

    const float tileSize = static_cast<float>(shadowTileSize);

    for (std::size_t tile = 0; tile < tileMatrices.size(); ++tile) {
        // Geometry outside a view is clipped before it is drawn, so it never spills into the neighbouring tiles.
        SDL_GPUViewport viewport{};
        viewport.x = static_cast<float>(tile % shadowAtlasColumns) * tileSize;
        viewport.y = static_cast<float>(tile / shadowAtlasColumns) * tileSize;
        viewport.w = tileSize;
        viewport.h = tileSize;
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(pass, &viewport);

        const Mat4& tileMatrix = tileMatrices[tile];

        for (const DrawItem& draw : frame.draws) {
            if (!draw.castsShadows) {
                continue;
            }

            const GpuMesh& mesh = bindMesh(draw.mesh);

            float transform[16]{};
            writeColumnMajor(tileMatrix * draw.model, transform);
            SDL_PushGPUVertexUniformData(commands, 0, transform, static_cast<Uint32>(sizeof(transform)));

            SDL_DrawGPUIndexedPrimitives(pass, mesh.getIndexCount(), 1, 0, 0, 0);
        }
    }

    SDL_EndGPURenderPass(pass);
    pass = nullptr;
}

/*
* Clears colour and depth, then starts the pass that draws the camera's view into the swapchain image.
*/
void GpuRenderer::beginMainPass(SDL_GPUTexture* swapchainTexture) {
    SDL_GPUColorTargetInfo target{};
    target.texture = swapchainTexture;
    target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = depthTexture;
    depthTarget.clear_depth = 1.0f;
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle = true;

    pass = SDL_BeginGPURenderPass(commands, &target, 1, &depthTarget);

    if (pass == nullptr) {
        throw gpuError("GPU render pass creation failed");
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
}

/*
* Handles are indices into the same numbering the AssetManager uses, so finding the GPU copy is one array access.
*/
const GpuMesh& GpuRenderer::bindMesh(MeshHandle meshHandle) {
    if (!meshHandle.isValid() || meshHandle.index >= meshes.size()) {
        throw std::out_of_range("Mesh handle has not been uploaded");
    }

    const GpuMesh& mesh = *meshes[meshHandle.index];

    SDL_GPUBufferBinding vertexBinding{};
    vertexBinding.buffer = mesh.getVertexBuffer();
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

    SDL_GPUBufferBinding indexBinding{};
    indexBinding.buffer = mesh.getIndexBuffer();
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    return mesh;
}

void GpuRenderer::drawMesh(MeshHandle meshHandle, const Mat4& model, const Material& material) {
    const GpuMesh& mesh = bindMesh(meshHandle);

    const Mat4 transform = viewProjection * model;

    // Once per object here rather than once per vertex in the shader.
    const Mat4 normals = normalMatrix(model);

    // The shader expects three consecutive column-major matrices:
    // the complete transform, the model matrix, and the normal matrix.
    float matrixData[48]{};
    writeColumnMajor(transform, matrixData);
    writeColumnMajor(model, matrixData + 16);
    writeColumnMajor(normals, matrixData + 32);

    SDL_PushGPUVertexUniformData(commands, 0, matrixData, static_cast<Uint32>(sizeof(matrixData)));

    const MaterialUniformData materialData = packMaterial(material);

    SDL_PushGPUFragmentUniformData(commands, 0, &materialData, static_cast<Uint32>(sizeof(materialData)));

    // No texture means plain colour: the shader multiplies the colour by white.
    const GpuTexture* selectedTexture = whiteTexture.get();

    if (material.texture.isValid()) {
        if (material.texture.index >= textures.size()) {
            throw std::out_of_range("drawMesh: texture handle has not been uploaded");
        }

        selectedTexture = textures[material.texture.index].get();
    }

    // Slot 0: the surface's colour texture. Slot 1: the shadow atlas, read with depth comparison.
    SDL_GPUTextureSamplerBinding textureBindings[2]{};
    textureBindings[0].texture = selectedTexture->getTexture();
    textureBindings[0].sampler = selectedTexture->getSampler();
    textureBindings[1].texture = shadowAtlas;
    textureBindings[1].sampler = shadowSampler;

    SDL_BindGPUFragmentSamplers(pass, 0, textureBindings, 2);

    SDL_DrawGPUIndexedPrimitives(pass, mesh.getIndexCount(), 1, 0, 0, 0);
}

void GpuRenderer::endFrame() {
    SDL_EndGPURenderPass(pass);
    pass = nullptr;

    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    commands = nullptr;

    if (!submitted) {
        throw gpuError("GPU submission failed");
    }
}

/*
* After an error mid-frame: close whatever is open and submit, because an acquired swapchain image must be submitted, not cancelled.
*/
void GpuRenderer::abandonFrame() noexcept {
    if (pass != nullptr) {
        SDL_EndGPURenderPass(pass);
        pass = nullptr;
    }

    if (commands != nullptr) {
        SDL_SubmitGPUCommandBuffer(commands);
        commands = nullptr;
    }
}

void GpuRenderer::createWhiteTexture() {
    const Uint8 whitePixel[4] = {255, 255, 255, 255};

    whiteTexture = std::make_unique<GpuTexture>(device, 1, 1, std::span<const Uint8>{whitePixel, 4}
    );
}

/*
* Uploads copy data with their own command buffer, so they must happen between frames.
* Each upload is appended, so the GPU copy gets the same index as its handle in the AssetManager.
*/
void GpuRenderer::uploadMesh(const IndexedMesh& mesh) {
    if (commands != nullptr) {
        throw std::logic_error("Upload meshes before beginning a frame");
    }

    meshes.push_back(std::make_unique<GpuMesh>(device, mesh));
}

void GpuRenderer::uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels) {
    if (commands != nullptr) {
        throw std::logic_error("Upload textures before beginning a frame");
    }

    textures.push_back(std::make_unique<GpuTexture>(device, width, height, pixels));
}

std::size_t GpuRenderer::getMeshCount() const {
    return meshes.size();
}

std::size_t GpuRenderer::getTextureCount() const {
    return textures.size();
}

/*
* Not every GPU and driver supports every mode, so check first and fall back to Vsync, which SDL guarantees.
*/
PresentMode GpuRenderer::setPresentMode(PresentMode requested) {
    if (commands != nullptr) {
        throw std::logic_error("Change the present mode between frames");
    }

    const auto toSdl = [](PresentMode mode) {
        switch (mode) {
        case PresentMode::Mailbox:   return SDL_GPU_PRESENTMODE_MAILBOX;
        case PresentMode::Immediate: return SDL_GPU_PRESENTMODE_IMMEDIATE;
        case PresentMode::Vsync:     return SDL_GPU_PRESENTMODE_VSYNC;
        }
        return SDL_GPU_PRESENTMODE_VSYNC;
    };

    PresentMode chosen = requested;

    if (!SDL_WindowSupportsGPUPresentMode(device, window, toSdl(chosen))) {
        chosen = PresentMode::Vsync;
    }

    if (!SDL_SetGPUSwapchainParameters(device, window, swapchainComposition, toSdl(chosen))) {
        throw gpuError("Could not set the present mode");
    }

    presentMode = chosen;
    return presentMode;
}

PresentMode GpuRenderer::getPresentMode() const {
    return presentMode;
}

float GpuRenderer::getFrameAspectRatio() const {
    if (depthWidth == 0 || depthHeight == 0) {
        throw std::logic_error("No frame dimensions are available");
    }

    return static_cast<float>(depthWidth) /
           static_cast<float>(depthHeight);
}
