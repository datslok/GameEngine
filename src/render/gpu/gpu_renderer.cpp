#include "render/gpu/gpu_renderer.h"
#include "render/gpu/debug_line_renderer.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/gpu_helpers.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "scene/culling.h"

#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

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
        debugLines = std::make_unique<DebugLineRenderer>(device, SDL_GetGPUSwapchainTextureFormat(device, window));
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
            2, // Colour texture, shadow atlas.
            1  // Shadow tiles.
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
* The atlas holds every shadow view, one per tile (packed each frame by planShadows). It is both drawn into (as a depth
* target) and read (as a texture). The sampler compares instead of returning depth: linear filtering then blends the
* four nearest comparisons, which already softens edges a little before the shader's 3x3 average.
* The pipeline draws depth only, from positions alone. Both sides of triangles are drawn, so open meshes (a plane) still
* cast shadows. There is no hardware depth bias: its slope-scaled part grows without limit on surfaces the light sees
* edge-on (a pillar's side seen from a lamp above it), and pushed whole pillars behind the ground, detaching their
* shadows. The shader's normal offset, measured in texels of each tile, handles acne instead.
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
    atlasInfo.width = shadowAtlasWidth;
    atlasInfo.height = shadowAtlasHeight;
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

    // Room for every tile there can be, so the buffers never need to grow; the upload copies only the tiles in use.
    SDL_GPUBufferCreateInfo tileBufferInfo{};
    tileBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    tileBufferInfo.size = static_cast<Uint32>(maxShadowTiles * sizeof(ShadowTileData));

    shadowTileBuffer = SDL_CreateGPUBuffer(device, &tileBufferInfo);

    if (shadowTileBuffer == nullptr) {
        throw gpuError("Shadow tile buffer creation failed");
    }

    SDL_GPUTransferBufferCreateInfo tileTransferInfo{};
    tileTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tileTransferInfo.size = tileBufferInfo.size;

    shadowTileTransfer = SDL_CreateGPUTransferBuffer(device, &tileTransferInfo);

    if (shadowTileTransfer == nullptr) {
        throw gpuError("Shadow tile transfer buffer creation failed");
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
        debugLines.reset();

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

        if (shadowTileBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(device, shadowTileBuffer);
            shadowTileBuffer = nullptr;
        }

        if (shadowTileTransfer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, shadowTileTransfer);
            shadowTileTransfer = nullptr;
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

    stats = RenderStats{};

    try {
        // Only now is the frame's size known, so the camera's lens is fitted to it here.
        Camera camera = frame.camera;
        camera.setAspectRatio(getFrameAspectRatio());

        // The projection follows OpenGL's depth range; the GPU's differs, and that is the renderer's business, so it is converted here.
        // View and projection are combined once per frame, not once per object.
        viewProjection = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix();
        const Frustum cameraFrustum = Frustum::fromClipMatrix(viewProjection, ClipDepth::ZeroToOne);

        // When there are more lights than seats, the ones that matter most here get them, remembering last frame's choice.
        const PrioritizedLighting prioritized = prioritizeLights(frame.lighting, frame.lightFocus, cameraFrustum, lightHistory, frame.frameSeconds);
        lightHistory = prioritized.history;

        stats.pointLights = frame.lighting.pointLights.size();
        stats.pointLightsInView = prioritized.pointLightsInView;
        stats.pointLightsLit = prioritized.lighting.pointLights.size();
        stats.pointLightsShadowed = static_cast<std::size_t>(std::count_if(
            prioritized.lighting.pointLights.begin(), prioritized.lighting.pointLights.end(),
            [](const PlacedPointLight& placed) { return placed.light.castsShadows; }));

        const LightUniformData lights = packLighting(prioritized.lighting, camera.getPosition());
        const ShadowPlan shadows = planShadows(prioritized.lighting, camera);

        // Copies must happen outside render passes, so the debug lines are uploaded first.
        debugLines->upload(commands, frame.debugLines, frame.debugScreenLines);
        uploadShadowTiles(shadows.tileData);

        if (!shadows.tiles.empty()) {
            drawShadows(frame, shadows.tiles);
        }

        beginMainPass(swapchainTexture);

        // Pushed uniform data stays in effect for every later draw, so the lights and shadow data are sent once per frame.
        SDL_PushGPUFragmentUniformData(commands, 1, &lights, static_cast<Uint32>(sizeof(lights)));
        SDL_PushGPUFragmentUniformData(commands, 2, &shadows.uniforms, static_cast<Uint32>(sizeof(shadows.uniforms)));
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &shadowTileBuffer, 1);

        // Only what the camera can see. Shadows chose their casters with each light's own view, above.
        const std::vector<std::size_t> visible = visibleDraws(frame.draws, cameraFrustum);

        for (std::size_t index : visible) {
            const DrawItem& draw = frame.draws[index];
            drawMesh(draw.mesh, draw.model, draw.material);
        }

        stats.drawn = visible.size();
        stats.culled = frame.draws.size() - visible.size();

        // Last, so they are drawn over the finished scene.
        debugLines->draw(commands, pass, viewProjection, depthWidth, depthHeight);
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
* The tiles' matrices and atlas rectangles go to a storage buffer, since there are too many for a uniform block.
* Copies must happen outside render passes, so this runs before the shadow pass. Cycling lets the GPU keep reading
* last frame's copy while this frame's is written.
*/
void GpuRenderer::uploadShadowTiles(const std::vector<ShadowTileData>& tileData) {
    if (tileData.empty()) {
        return;
    }

    const Uint32 byteCount = static_cast<Uint32>(tileData.size() * sizeof(ShadowTileData));
    void* destination = SDL_MapGPUTransferBuffer(device, shadowTileTransfer, true);

    if (destination == nullptr) {
        throw gpuError("Shadow tile transfer buffer mapping failed");
    }

    std::memcpy(destination, tileData.data(), byteCount);
    SDL_UnmapGPUTransferBuffer(device, shadowTileTransfer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commands);

    if (copyPass == nullptr) {
        throw gpuError("Shadow tile copy pass creation failed");
    }

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = shadowTileTransfer;

    SDL_GPUBufferRegion target{};
    target.buffer = shadowTileBuffer;
    target.size = byteCount;

    SDL_UploadToGPUBuffer(copyPass, &source, &target, true);
    SDL_EndGPUCopyPass(copyPass);
}

/*
* The shadow maps: the scene drawn once per shadow view (six for each point light, one for each spotlight or directional
* light), each into its own tile of the atlas (the viewport picks the tile). Only depth is kept, so each texel holds how
* far the nearest surface is from the light in that direction. Meshes that do not cast shadows are left out.
*/
void GpuRenderer::drawShadows(const FrameDescription& frame, const std::vector<ShadowTile>& tiles) {
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

    for (const ShadowTile& tile : tiles) {
        // A cube face that sees nothing on screen has no square in the atlas.
        if (tile.size == 0) {
            ++stats.shadowTilesSkipped;
            continue;
        }

        ++stats.shadowTilesDrawn;

        // Geometry outside a view is clipped before it is drawn, so it never spills into the neighbouring tiles.
        SDL_GPUViewport viewport{};
        viewport.x = static_cast<float>(tile.x);
        viewport.y = static_cast<float>(tile.y);
        viewport.w = static_cast<float>(tile.size);
        viewport.h = static_cast<float>(tile.size);
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(pass, &viewport);

        const Mat4& tileMatrix = tile.matrix;

        // The light's own view decides, not the camera's: things off screen can still throw shadows into view.
        const std::vector<std::size_t> casters = shadowCasters(frame.draws, Frustum::fromClipMatrix(tileMatrix, ClipDepth::ZeroToOne));
        stats.shadowDrawn += casters.size();
        stats.shadowCulled += frame.draws.size() - casters.size();

        for (std::size_t index : casters) {
            const DrawItem& draw = frame.draws[index];
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

const RenderStats& GpuRenderer::getLastFrameStats() const {
    return stats;
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
