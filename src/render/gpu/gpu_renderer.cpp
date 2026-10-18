#include "render/gpu/gpu_renderer.h"
#include "render/gpu/brdf.h"
#include "render/gpu/debug_line_renderer.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/gpu_helpers.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/tone_map_uniforms.h"
#include "scene/culling.h"

#include "imgui/imgui_impl_sdlgpu3.h"

#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {
    // The lit scene's format: 16-bit floats keep about three significant digits from a millionth up to 65504, plenty for
    // light far brighter than white, at half the memory of 32-bit floats. Alpha is unused but there is no 3-channel format.
    constexpr SDL_GPUTextureFormat sceneColourFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_GPUPresentMode sdlPresentMode(PresentMode mode) {
        switch (mode) {
        case PresentMode::Mailbox:   return SDL_GPU_PRESENTMODE_MAILBOX;
        case PresentMode::Immediate: return SDL_GPU_PRESENTMODE_IMMEDIATE;
        case PresentMode::Vsync:     return SDL_GPU_PRESENTMODE_VSYNC;
        }
        return SDL_GPU_PRESENTMODE_VSYNC;
    }
}

GpuRenderer::GpuRenderer(SDL_Window* window, bool validation):
    window(window)
{
    if (window == nullptr) {
        throw std::invalid_argument("GpuRenderer requires a window");
    }

    try {
        // Select Vulkan and the shader format we will use later.
        device = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV,
            validation,
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
        createToneMapResources();
        bloom = std::make_unique<BloomRenderer>(device);
        createScreenPipelines();
        createWhiteTexture();
        createBrdfTable();
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

    setSwapchainComposition(SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR);
    sdrComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
}

// Changing the composition keeps the present mode.
void GpuRenderer::setSwapchainComposition(SDL_GPUSwapchainComposition composition) {
    if (!SDL_SetGPUSwapchainParameters(device, window, composition, sdlPresentMode(presentMode))) {
        throw gpuError("Could not change the swapchain's composition");
    }

    swapchainComposition = composition;
}

/*
* Windows reports HDR per monitor, and it changes while the game runs (HDR switched in Settings, the window dragged to
* another screen), so it is read every frame; the window properties are a cheap lookup. Whether the GPU can present a
* float image is asked only when HDR comes on, since that query talks to the driver.
*/
DisplayCapabilities GpuRenderer::readDisplayCapabilities() const {
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);

    DisplayCapabilities capabilities;
    capabilities.hdrEnabled = SDL_GetBooleanProperty(properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    capabilities.hdrHeadroom = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    capabilities.sdrWhiteLevel = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);

    if (capabilities.hdrEnabled == displayCapabilities.hdrEnabled) {
        capabilities.hdrSwapchainSupported = displayCapabilities.hdrSwapchainSupported;
    }
    else if (capabilities.hdrEnabled) {
        capabilities.hdrSwapchainSupported =
            SDL_WindowSupportsGPUSwapchainComposition(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    }

    return capabilities;
}

/*
* Between frames: decide how this frame reaches the screen, and when HDR turns on or off, swap the swapchain to the
* float (scRGB) or sRGB format and rebuild the pipelines that draw into it. The GPU must be idle first, since the old
* pipelines may still be in use. If the driver refuses the HDR swapchain, it is marked unsupported so it is not tried
* again every frame, and the frame goes out in SDR.
*/
void GpuRenderer::updateDisplayOutput(const DisplaySettings& settings) {
    displayCapabilities = readDisplayCapabilities();
    DisplayOutput next = chooseDisplayOutput(settings, displayCapabilities);

    if (next.hdr != displayOutput.hdr) {
        SDL_WaitForGPUIdle(device);

        try {
            setSwapchainComposition(next.hdr ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR : sdrComposition);
        }
        catch (const std::runtime_error& error) {
            if (!next.hdr) {
                throw;
            }

            SDL_Log("HDR output unavailable: %s", error.what());
            displayCapabilities.hdrSwapchainSupported = false;
            next = DisplayOutput{};
        }

        createScreenPipelines();

        if (next.hdr) {
            SDL_Log("HDR output on: peak %.2f times SDR white, SDR white at %.2f (%.0f nits)",
                    next.peakBrightness, next.whiteLevel, next.whiteLevel * scrgbWhiteNits);
        }
        else {
            SDL_Log("HDR output off");
        }
    }

    displayOutput = next;
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
            5, // Colour texture, shadow atlas (compared), shadow atlas (raw depths), metallic-roughness texture, BRDF table.
            1  // Shadow tiles.
        );

        // The scene image, not the screen: tone mapping turns it into the screen image afterwards.
        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = sceneColourFormat;

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
        // Reversed-Z: nearer surfaces have larger depths, so the nearest is the greatest.
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
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
    // Lit when our depth is no further from the light than the nearest surface it saw: with reversed-Z, at least as large.
    samplerInfo.enable_compare = true;
    samplerInfo.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;

    shadowSampler = SDL_CreateGPUSampler(device, &samplerInfo);

    if (shadowSampler == nullptr) {
        throw gpuError("Shadow sampler creation failed");
    }

    // The same atlas read as raw depths, for the soft shadow blocker search: exact texels, no comparison.
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.enable_compare = false;

    shadowDepthSampler = SDL_CreateGPUSampler(device, &samplerInfo);

    if (shadowDepthSampler == nullptr) {
        throw gpuError("Shadow depth sampler creation failed");
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
    SDL_GPUShader* clearVertexShader = nullptr;

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
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER; // reversed-Z, as in the main pass

        shadowPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (shadowPipeline == nullptr) {
            throw gpuError("Shadow pipeline creation failed");
        }

        // The tile reset: one triangle made in the shader (no vertex input), and a depth test that always passes, so it
        // overwrites the whole tile with the far depth.
        clearVertexShader = loadShader(device, "assets/shaders/fullscreen_triangle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        info.vertex_shader = clearVertexShader;
        info.vertex_input_state = SDL_GPUVertexInputState{};
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;

        shadowClearPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (shadowClearPipeline == nullptr) {
            throw gpuError("Shadow tile reset pipeline creation failed");
        }
    }
    catch (...) {
        if (clearVertexShader != nullptr) {
            SDL_ReleaseGPUShader(device, clearVertexShader);
        }

        if (fragmentShader != nullptr) {
            SDL_ReleaseGPUShader(device, fragmentShader);
        }

        if (vertexShader != nullptr) {
            SDL_ReleaseGPUShader(device, vertexShader);
        }

        throw;
    }

    SDL_ReleaseGPUShader(device, clearVertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);
    SDL_ReleaseGPUShader(device, vertexShader);
}

/*
* The final pass draws one full-screen triangle (no vertex input, no depth) whose shader reads the scene image texel for
* texel and writes the tone mapped colour to the screen.
*/
void GpuRenderer::createToneMapResources() {
    const SDL_GPUTextureUsageFlags sceneUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    if (!SDL_GPUTextureSupportsFormat(device, sceneColourFormat, SDL_GPU_TEXTURETYPE_2D, sceneUsage)) {
        throw std::runtime_error("This GPU cannot draw into a 16-bit float texture, which HDR rendering needs");
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    sceneSampler = SDL_CreateGPUSampler(device, &samplerInfo);

    if (sceneSampler == nullptr) {
        throw gpuError("Scene sampler creation failed");
    }
}

/*
* The pipelines that draw into the swapchain image (the tone mapping pass, the debug lines and the overlay) are built for its
* format, which changes when HDR output is switched on or off (8-bit sRGB or 16-bit float). Any old ones are released
* first; the caller makes sure the GPU no longer uses them.
*/
void GpuRenderer::createScreenPipelines() {
    if (toneMapPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device, toneMapPipeline);
        toneMapPipeline = nullptr;
    }

    debugLines.reset();
    overlay.reset();

    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/fullscreen_triangle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        fragmentShader = loadShader(device, "assets/shaders/tone_map.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2);

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader;
        info.fragment_shader = fragmentShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &colourTarget;
        info.target_info.has_depth_stencil_target = false;

        toneMapPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (toneMapPipeline == nullptr) {
            throw gpuError("Tone mapping pipeline creation failed");
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

    debugLines = std::make_unique<DebugLineRenderer>(device, SDL_GetGPUSwapchainTextureFormat(device, window));
    overlay = std::make_unique<OverlayRenderer>(device, SDL_GetGPUSwapchainTextureFormat(device, window));
}

GpuRenderer::~GpuRenderer() {
    cleanup();
}

/*
* The scene image and its depth follow the acquired frame's size, so they are remade when the window is resized. Both
* are created before either old one is released, so a failure leaves the previous pair intact.
*/
void GpuRenderer::ensureFrameTextures(Uint32 width, Uint32 height) {
    if (depthTexture != nullptr &&
        frameWidth == width &&
        frameHeight == height) {
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

    SDL_GPUTexture* depthReplacement = SDL_CreateGPUTexture(device, &info);

    if (depthReplacement == nullptr) {
        throw gpuError("Depth texture creation failed");
    }

    info.format = sceneColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    SDL_GPUTexture* sceneReplacement = SDL_CreateGPUTexture(device, &info);

    if (sceneReplacement == nullptr) {
        SDL_ReleaseGPUTexture(device, depthReplacement);
        throw gpuError("Scene colour texture creation failed");
    }

    if (depthTexture != nullptr) {
        SDL_ReleaseGPUTexture(device, depthTexture);
    }

    if (sceneColour != nullptr) {
        SDL_ReleaseGPUTexture(device, sceneColour);
    }

    depthTexture = depthReplacement;
    sceneColour = sceneReplacement;
    frameWidth = width;
    frameHeight = height;
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
    brdfTable.reset();
        overlay.reset();
        debugLines.reset();
        bloom.reset();

        if (depthTexture != nullptr) {
            SDL_ReleaseGPUTexture(device, depthTexture);
            depthTexture = nullptr;
            frameWidth = 0;
            frameHeight = 0;
        }

        if (sceneColour != nullptr) {
            SDL_ReleaseGPUTexture(device, sceneColour);
            sceneColour = nullptr;
        }

        if (sceneSampler != nullptr) {
            SDL_ReleaseGPUSampler(device, sceneSampler);
            sceneSampler = nullptr;
        }

        if (toneMapPipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, toneMapPipeline);
            toneMapPipeline = nullptr;
        }

        if (pipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            pipeline = nullptr;
        }

        if (shadowPipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, shadowPipeline);
            shadowPipeline = nullptr;
        }

        if (shadowClearPipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device, shadowClearPipeline);
            shadowClearPipeline = nullptr;
        }

        if (shadowSampler != nullptr) {
            SDL_ReleaseGPUSampler(device, shadowSampler);
            shadowSampler = nullptr;
        }

        if (shadowDepthSampler != nullptr) {
            SDL_ReleaseGPUSampler(device, shadowDepthSampler);
            shadowDepthSampler = nullptr;
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
* ImGui's backend draws into the overlay image, whose format never changes, so switching HDR on or off does not touch it.
*/
void GpuRenderer::startDebugUi() {
    if (debugUiStarted) {
        return;
    }

    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = device;
    info.ColorTargetFormat = overlayFormat;

    if (!ImGui_ImplSDLGPU3_Init(&info)) {
        throw gpuError("Debug UI renderer start-up failed");
    }

    debugUiStarted = true;
}

void GpuRenderer::stopDebugUi() {
    if (!debugUiStarted) {
        return;
    }

    SDL_WaitForGPUIdle(device);
    ImGui_ImplSDLGPU3_Shutdown();
    debugUiStarted = false;
}


/*
* The whole frame arrives at once, so the renderer decides how to draw it: first the shadow-casting light's view of the
* scene into the shadow atlas, then the camera's view, which reads that atlas. Each view draws only what its frustum can see (culling).
* If anything fails part-way, the frame is still submitted (a swapchain image was acquired), then the error is passed on.
*/
bool GpuRenderer::render(const FrameDescription& frame, const RenderSettings& settings, ImDrawData* ui) {
    // Before acquiring an image, since switching HDR on or off replaces the swapchain.
    updateDisplayOutput(settings.display);

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
        // View and projection are combined once per frame, not once per object. The world-space version decides what
        // is seen; the main pass draws with the camera-relative one (see camera_relative.h).
        viewProjection = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix();
        relativeViewProjection = cameraRelativeViewProjection(camera);
        cameraOrigin = camera.getPosition();
        const Frustum cameraFrustum = Frustum::fromClipMatrix(viewProjection, ClipDepth::OneToZero);

        // When there are more lights than seats, the ones that matter most here get them, remembering last frame's choice.
        const PrioritizedLighting prioritized = prioritizeLights(frame.lighting, frame.lightFocus, cameraFrustum, lightHistory, frame.frameSeconds,
                                                                 settings.lights);
        lightHistory = prioritized.history;

        stats.pointLights = frame.lighting.pointLights.size();
        stats.pointLightsInView = prioritized.pointLightsInView;
        stats.pointLightsLit = prioritized.lighting.pointLights.size();
        stats.pointLightsShadowed = static_cast<std::size_t>(std::count_if(
            prioritized.lighting.pointLights.begin(), prioritized.lighting.pointLights.end(),
            [](const PlacedPointLight& placed) { return placed.light.castsShadows; }));

        const LightUniformData lights = packLighting(prioritized.lighting, cameraOrigin);
        const ShadowPlan shadows = planShadows(prioritized.lighting, camera, shadowLayout, settings.shadows);

        // Copies must happen outside render passes, so the debug lines are uploaded first.
        debugLines->upload(commands, frame.debugLines, frame.debugScreenLines, displayOutput.whiteLevel);
        uploadShadowTiles(shadows.tileData);

        // The UI goes into its own image now, while no pass is open (its backend copies its buffers first).
        if (ui != nullptr) {
            overlay->drawUi(commands, ui, frameWidth, frameHeight);
        }

        if (!shadows.tiles.empty()) {
            drawShadows(frame, shadows.tiles);
        }

        beginMainPass();

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

        SDL_EndGPURenderPass(pass);
        pass = nullptr;

        // The scattered light, from the finished scene image, before tone mapping mixes it in.
        const BloomComposite bloomMix = bloom->draw(commands, sceneColour, frameWidth, frameHeight, settings.bloom);

        beginToneMapPass(swapchainTexture, settings.toneMapping, bloomMix);

        // Last, so they are drawn over the finished image, in exactly their own colours.
        debugLines->draw(commands, pass, viewProjection, frameWidth, frameHeight);

        // The UI over everything, the debug lines included, so its windows stay readable.
        if (ui != nullptr) {
            overlay->composite(commands, pass, displayOutput.whiteLevel);
        }
    }
    catch (...) {
        // The shadow pass may have stopped part-way, so no cached tile can be trusted.
        shadowMemory.forgetAll();
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
    Uint32 acquiredWidth = 0;
    Uint32 acquiredHeight = 0;

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchainTexture, &acquiredWidth, &acquiredHeight)) {
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
        ensureFrameTextures(acquiredWidth, acquiredHeight);
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
* The shadow maps: the scene drawn once per shadow view (up to six for each point light, one for each spotlight or
* directional light), each into its own tile of the atlas (the viewport picks the tile). Only depth is kept, so each texel
* holds how far the nearest surface is from the light in that direction. Meshes that do not cast shadows are left out.
*
* The atlas keeps its contents between frames, so a tile whose view and casters are the same as when it was last drawn
* is left alone (shadow caching): a lamp standing beside still pillars costs nothing after its first frame. A tile that
* must be drawn again is first reset to the far depth, since only its square is being redrawn. When nothing changed,
* there is no shadow pass at all.
*/
void GpuRenderer::drawShadows(const FrameDescription& frame, const std::vector<ShadowTile>& tiles) {
    struct TileToDraw {
        const ShadowTile* tile;
        std::vector<std::size_t> casters;
        std::uint64_t signature;
    };

    std::vector<TileToDraw> toDraw;

    for (const ShadowTile& tile : tiles) {
        // A cube face that sees nothing on screen has no square in the atlas.
        if (tile.size == 0) {
            ++stats.shadowTilesSkipped;
            continue;
        }

        // The light's own view decides, not the camera's: things off screen can still throw shadows into view.
        std::vector<std::size_t> casters = shadowCasters(frame.draws, Frustum::fromClipMatrix(tile.matrix, ClipDepth::OneToZero));
        const std::uint64_t signature = shadowCasterSignature(frame.draws, casters);

        if (shadowMemory.isCurrent(tile, signature)) {
            ++stats.shadowTilesCached;
            continue;
        }

        toDraw.push_back(TileToDraw{&tile, std::move(casters), signature});
    }

    if (toDraw.empty()) {
        return;
    }

    // Load, not clear: the tiles not drawn this frame must keep their images. No cycling either, since a fresh copy of
    // the texture would start empty.
    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = shadowAtlas;
    depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
    depthTarget.store_op = SDL_GPU_STOREOP_STORE; // The main pass reads it.
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle = false;

    pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depthTarget);

    if (pass == nullptr) {
        throw gpuError("Shadow pass creation failed");
    }

    for (const TileToDraw& drawing : toDraw) {
        const ShadowTile& tile = *drawing.tile;
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

        // Reset the tile, then draw its casters.
        SDL_BindGPUGraphicsPipeline(pass, shadowClearPipeline);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_BindGPUGraphicsPipeline(pass, shadowPipeline);

        stats.shadowDrawn += drawing.casters.size();
        stats.shadowCulled += frame.draws.size() - drawing.casters.size();

        for (std::size_t index : drawing.casters) {
            const DrawItem& draw = frame.draws[index];
            const GpuMesh& mesh = bindMesh(draw.mesh);

            float transform[16]{};
            writeColumnMajor(tile.matrix * draw.model, transform);
            SDL_PushGPUVertexUniformData(commands, 0, transform, static_cast<Uint32>(sizeof(transform)));

            SDL_DrawGPUIndexedPrimitives(pass, mesh.getIndexCount(), 1, 0, 0, 0);
        }

        shadowMemory.remember(tile, drawing.signature);
    }

    SDL_EndGPURenderPass(pass);
    pass = nullptr;
}

/*
* Clears colour and depth, then starts the pass that draws the camera's view into the scene image. Cycling lets the GPU
* hand out a fresh copy if last frame's is still being read.
*/
void GpuRenderer::beginMainPass() {
    SDL_GPUColorTargetInfo target{};
    target.texture = sceneColour;
    target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE; // The tone mapping pass reads it.
    target.cycle = true;

    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = depthTexture;
    depthTarget.clear_depth = gpuFarDepth;
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

    // The object placed relative to the camera before any matrix multiplies it (camera-relative rendering).
    const Mat4 relativeModel = relativeTo(model, cameraOrigin);
    const Mat4 transform = relativeViewProjection * relativeModel;

    // Once per object here rather than once per vertex in the shader. Moving the origin does not turn anything, so
    // the world model matrix gives the same normals.
    const Mat4 normals = normalMatrix(model);

    // The shader expects three consecutive column-major matrices:
    // the complete transform, the camera-relative model matrix, and the normal matrix.
    float matrixData[48]{};
    writeColumnMajor(transform, matrixData);
    writeColumnMajor(relativeModel, matrixData + 16);
    writeColumnMajor(normals, matrixData + 32);

    SDL_PushGPUVertexUniformData(commands, 0, matrixData, static_cast<Uint32>(sizeof(matrixData)));

    const MaterialUniformData materialData = packMaterial(material);

    SDL_PushGPUFragmentUniformData(commands, 0, &materialData, static_cast<Uint32>(sizeof(materialData)));

    const GpuTexture& colourTexture = textureOrWhite(material.texture);
    const GpuTexture& metallicRoughnessTexture = textureOrWhite(material.metallicRoughnessTexture);

    // Slot 0: the surface's colour texture. Slot 1: the shadow atlas, read with depth comparison. Slot 2: the same atlas,
    // read as raw depths. Slot 3: the surface's metallic-roughness texture. Slot 4: the BRDF table.
    SDL_GPUTextureSamplerBinding textureBindings[5]{};
    textureBindings[0].texture = colourTexture.getTexture();
    textureBindings[0].sampler = colourTexture.getSampler();
    textureBindings[1].texture = shadowAtlas;
    textureBindings[1].sampler = shadowSampler;
    textureBindings[2].texture = shadowAtlas;
    textureBindings[2].sampler = shadowDepthSampler;
    textureBindings[3].texture = metallicRoughnessTexture.getTexture();
    textureBindings[3].sampler = metallicRoughnessTexture.getSampler();
    textureBindings[4].texture = brdfTable->getTexture();
    textureBindings[4].sampler = brdfTable->getSampler();

    SDL_BindGPUFragmentSamplers(pass, 0, textureBindings, 5);

    SDL_DrawGPUIndexedPrimitives(pass, mesh.getIndexCount(), 1, 0, 0, 0);
}

/*
* Starts the final pass, into the swapchain image, and fills it: the full-screen triangle runs the tone mapping shader on
* every pixel. The pass stays open for the debug lines. The image is wholly overwritten, so its old contents are not loaded.
* The display output (chosen before the frame) gives the curve's peak, the white level and whether to dither. Without
* bloom this frame the shader never reads its texture, but the slot must still hold one, so the scene stands in.
*/
void GpuRenderer::beginToneMapPass(SDL_GPUTexture* swapchainTexture, const ToneMappingSettings& settings, const BloomComposite& bloomMix) {
    const ToneMapUniformData toneMapping = packToneMapping(settings, displayOutput, bloomMix);

    SDL_GPUColorTargetInfo target{};
    target.texture = swapchainTexture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);

    if (pass == nullptr) {
        throw gpuError("Tone mapping pass creation failed");
    }

    SDL_BindGPUGraphicsPipeline(pass, toneMapPipeline);
    SDL_PushGPUFragmentUniformData(commands, 0, &toneMapping, static_cast<Uint32>(sizeof(toneMapping)));

    SDL_GPUTexture* bloomTexture = bloom->getResult();

    // Slot 0: the scene, exact texels. Slot 1: the bloom, filtered (it is half the screen's size).
    SDL_GPUTextureSamplerBinding bindings[2]{};
    bindings[0].texture = sceneColour;
    bindings[0].sampler = sceneSampler;
    bindings[1].texture = bloomTexture != nullptr ? bloomTexture : sceneColour;
    bindings[1].sampler = bloom->getSampler();
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
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

    whiteTexture = std::make_unique<GpuTexture>(device, 1, 1, std::span<const Uint8>{whitePixel, 4}, surfaceTextureDescription(TextureEncoding::Srgb));
}

// No texture means plain colour or plain factors: the shader multiplies them by white.
const GpuTexture& GpuRenderer::textureOrWhite(TextureHandle handle) const {
    if (!handle.isValid()) {
        return *whiteTexture;
    }

    if (handle.index >= textures.size()) {
        throw std::out_of_range("drawMesh: texture handle has not been uploaded");
    }

    return *textures[handle.index];
}

/*
* Two 16-bit channels per texel (scale and bias, 0..1). 8 bits would put errors of a few tenths of a percent into every
* surface's reflectance; 16-bit UNORM is exact enough and smaller than floats.
*/
void GpuRenderer::createBrdfTable() {
    constexpr SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R16G16_UNORM;

    if (!SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        throw std::runtime_error("This GPU cannot sample a 16-bit two-channel texture, which physically based shading needs");
    }

    const BrdfTable table = buildBrdfTable();
    const std::vector<std::uint16_t> texels = brdfTableTexels(table);

    GpuTextureDescription description;
    description.format = format;
    description.bytesPerTexel = 2 * sizeof(std::uint16_t);
    description.surface = false;

    brdfTable = std::make_unique<GpuTexture>(
        device, table.size, table.size,
        std::span<const Uint8>{reinterpret_cast<const Uint8*>(texels.data()), texels.size() * sizeof(std::uint16_t)},
        description
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

void GpuRenderer::uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding) {
    if (commands != nullptr) {
        throw std::logic_error("Upload textures before beginning a frame");
    }

    textures.push_back(std::make_unique<GpuTexture>(device, width, height, pixels, surfaceTextureDescription(encoding)));
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

    PresentMode chosen = requested;

    if (!SDL_WindowSupportsGPUPresentMode(device, window, sdlPresentMode(chosen))) {
        chosen = PresentMode::Vsync;
    }

    if (!SDL_SetGPUSwapchainParameters(device, window, swapchainComposition, sdlPresentMode(chosen))) {
        throw gpuError("Could not set the present mode");
    }

    presentMode = chosen;
    return presentMode;
}

const RenderStats& GpuRenderer::getLastFrameStats() const {
    return stats;
}

const DisplayOutput& GpuRenderer::getDisplayOutput() const {
    return displayOutput;
}

PresentMode GpuRenderer::getPresentMode() const {
    return presentMode;
}

float GpuRenderer::getFrameAspectRatio() const {
    if (frameWidth == 0 || frameHeight == 0) {
        throw std::logic_error("No frame dimensions are available");
    }

    return static_cast<float>(frameWidth) /
           static_cast<float>(frameHeight);
}
