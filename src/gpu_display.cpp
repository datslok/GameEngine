#include "gpu_display.h"

#include <stdexcept>
#include <string>
#include <cstddef>

namespace {
    std::runtime_error gpuError(const char* message) {
        return std::runtime_error(
            std::string{message} + ": " + SDL_GetError()
        );
    }
    SDL_GPUShader* loadShader(
        SDL_GPUDevice* device,
        const char* filename,
        SDL_GPUShaderStage stage,
        Uint32 uniformBufferCount
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

GpuDisplay::GpuDisplay(const char* title, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument(
            "Display dimensions must be positive"
        );
    }

    try {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            throw gpuError("SDL initialization failed");
        }

        videoInitialized = true;

        // Select Vulkan and the shader format we will use later.
        device = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV,
            true,
            "vulkan"
        );

        if (device == nullptr) {
            throw gpuError("GPU device creation failed");
        }

        window = SDL_CreateWindow(title, width, height, 0);

        if (window == nullptr) {
            throw gpuError("Window creation failed");
        }

        // Connect this window to the GPU device.
        if (!SDL_ClaimWindowForGPUDevice(device, window)) {
            throw gpuError("GPU window claim failed");
        }

        windowClaimed = true;
        createPipeline();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

void GpuDisplay::createPipeline() {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(
            device,
            "assets/shaders/triangle.vert.spv",
            SDL_GPU_SHADERSTAGE_VERTEX,
            1
        );

        fragmentShader = loadShader(
            device,
            "assets/shaders/triangle.frag.spv",
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            1
        );

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format =
            SDL_GetGPUSwapchainTextureFormat(device, window);


        SDL_GPUVertexBufferDescription vertexDescription{};
        vertexDescription.slot = 0;
        vertexDescription.pitch = static_cast<Uint32>(sizeof(GpuVertex));
        vertexDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attributes[2]{};

        // Position: shader input location 0.
        attributes[0].location = 0;
        attributes[0].buffer_slot = 0;
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset = static_cast<Uint32>(offsetof(GpuVertex, x));

        // Surface normal.
        attributes[1].location = 1;
        attributes[1].buffer_slot = 0;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].offset = static_cast<Uint32>(offsetof(GpuVertex, nx));

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
        info.vertex_input_state.num_vertex_attributes = 2;
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

GpuDisplay::~GpuDisplay() {
    cleanup();
}

void GpuDisplay::ensureDepthTexture(Uint32 width, Uint32 height) {
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

void GpuDisplay::cleanup() noexcept {
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

        if (windowClaimed) {
            SDL_ReleaseWindowFromGPUDevice(device, window);
            windowClaimed = false;
        }

        SDL_DestroyGPUDevice(device);
        device = nullptr;
    }

    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    if (videoInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        videoInitialized = false;
    }
}

SDL_GPUDevice* GpuDisplay::getDevice() const {
    return device;
}

bool GpuDisplay::processEvents() {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window)) {
            return false;
        }
    }

    return true;
}

bool GpuDisplay::beginFrame(float red, float green, float blue) {
    if (commands != nullptr) {
        throw std::logic_error(
            "Finish the current frame before beginning another"
        );
    }

    commands = SDL_AcquireGPUCommandBuffer(device);

    if (commands == nullptr) {
        throw gpuError("GPU command buffer acquisition failed");
    }

    // Get the image that will become the next displayed frame.
    SDL_GPUTexture* swapchainTexture = nullptr;
    Uint32 frameWidth = 0;
    Uint32 frameHeight = 0;

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            commands,
            window,
            &swapchainTexture,
            &frameWidth,
            &frameHeight)) {

        const std::runtime_error error =
            gpuError("Swapchain acquisition failed");

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
        return false;
    }

    try {
        ensureDepthTexture(frameWidth, frameHeight);
    }
    catch (...) {
        // A swapchain image has been acquired, so submit rather than cancel.
        SDL_SubmitGPUCommandBuffer(commands);
        commands = nullptr;
        throw;
    }

    SDL_GPUColorTargetInfo target{};
    target.texture = swapchainTexture;
    target.clear_color = SDL_FColor{red, green, blue, 1.0f};
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
        const std::runtime_error error =
            gpuError("GPU render pass creation failed");

        SDL_SubmitGPUCommandBuffer(commands);
        commands = nullptr;
        throw error;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
        return true;
    }

void GpuDisplay::drawMesh(const GpuMesh& mesh, const Mat4& model, const Mat4& viewProjection, Pixel colour) {
    if (commands == nullptr || pass == nullptr) {
        throw std::logic_error("drawMesh requires an active frame");
    }

    SDL_GPUBufferBinding vertexBinding{};
    vertexBinding.buffer = mesh.getVertexBuffer();
    vertexBinding.offset = 0;

    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

    SDL_GPUBufferBinding indexBinding{};
    indexBinding.buffer = mesh.getIndexBuffer();
    indexBinding.offset = 0;

    SDL_BindGPUIndexBuffer(
        pass,
        &indexBinding,
        SDL_GPU_INDEXELEMENTSIZE_32BIT
    );

    const Mat4 transform = viewProjection * model;

    // The shader expects two consecutive column-major matrices:
    // the complete transform, followed by the model matrix.
    float matrixData[32]{};

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrixData[column * 4 + row] =
                transform.values[row][column];

            matrixData[16 + column * 4 + row] =
                model.values[row][column];
        }
    }

    SDL_PushGPUVertexUniformData(
        commands,
        0,
        matrixData,
        static_cast<Uint32>(sizeof(matrixData))
    );

    // Convert our byte colour channels to the shader's 0–1 range.
    const float colourData[4] = {
        static_cast<float>(colour.r) / 255.0f,
        static_cast<float>(colour.g) / 255.0f,
        static_cast<float>(colour.b) / 255.0f,
        1.0f
    };

    SDL_PushGPUFragmentUniformData(commands, 0, colourData, static_cast<Uint32>(sizeof(colourData)));

    SDL_DrawGPUIndexedPrimitives(pass, mesh.getIndexCount(), 1, 0, 0, 0);
}

void GpuDisplay::endFrame() {
    if (commands == nullptr || pass == nullptr) {
        throw std::logic_error("endFrame requires an active frame");
    }

    SDL_EndGPURenderPass(pass);
    pass = nullptr;

    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    commands = nullptr;

    if (!submitted) {
        throw gpuError("GPU submission failed");
    }
}