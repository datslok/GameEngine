#include "render/gpu/overlay_renderer.h"
#include "render/gpu/gpu_helpers.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdlgpu3.h"

#include <stdexcept>

namespace {
    // Mirrors the OverlayData block in overlay.frag (std140, one vec4).
    struct OverlayUniformData {
        float display[4]; // x: what SDR white is written as
    };

    static_assert(sizeof(OverlayUniformData) == 16, "OverlayUniformData must match the shader's OverlayData block");
}

OverlayRenderer::OverlayRenderer(SDL_GPUDevice* device, SDL_GPUTextureFormat screenFormat):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("OverlayRenderer requires a GPU device");
    }

    try {
        createPipeline(screenFormat);

        // Exact texels: the overlay is the screen's size, one texel per pixel.
        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

        sampler = SDL_CreateGPUSampler(device, &samplerInfo);

        if (sampler == nullptr) {
            throw gpuError("Overlay sampler creation failed");
        }
    }
    catch (...) {
        cleanup();
        throw;
    }
}

OverlayRenderer::~OverlayRenderer() {
    cleanup();
}

/*
* A full-screen triangle that blends the premultiplied overlay over the screen: what is already there is kept in
* proportion to how little the overlay covers it (src + dst * (1 - src alpha)).
*/
void OverlayRenderer::createPipeline(SDL_GPUTextureFormat screenFormat) {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/fullscreen_triangle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        fragmentShader = loadShader(device, "assets/shaders/overlay.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = screenFormat;
        colourTarget.blend_state.enable_blend = true;
        colourTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colourTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colourTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colourTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colourTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colourTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

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

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (pipeline == nullptr) {
            throw gpuError("Overlay pipeline creation failed");
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

// The new image is made before the old one is released, so a failure leaves the old one in place.
void OverlayRenderer::ensureImage(Uint32 width, Uint32 height) {
    if (image != nullptr && imageWidth == width && imageHeight == height) {
        return;
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = overlayFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device, &info);

    if (replacement == nullptr) {
        throw gpuError("Overlay image creation failed");
    }

    if (image != nullptr) {
        SDL_ReleaseGPUTexture(device, image);
    }

    image = replacement;
    imageWidth = width;
    imageHeight = height;
}

/*
* ImGui's backend copies its vertices, indices and any changed font textures in its own copy pass (PrepareDrawData),
* which is why this must run outside render passes; then it draws into the overlay, cleared to transparent so whatever
* the UI does not cover leaves the screen as it is.
*/
void OverlayRenderer::drawUi(SDL_GPUCommandBuffer* commands, ImDrawData* drawData, Uint32 frameWidth, Uint32 frameHeight) {
    ensureImage(frameWidth, frameHeight);
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commands);

    SDL_GPUColorTargetInfo target{};
    target.texture = image;
    target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);

    if (pass == nullptr) {
        throw gpuError("Overlay pass creation failed");
    }

    ImGui_ImplSDLGPU3_RenderDrawData(drawData, commands, pass);
    SDL_EndGPURenderPass(pass);
}

void OverlayRenderer::composite(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, float whiteLevel) {
    if (image == nullptr) {
        return;
    }

    const OverlayUniformData uniforms{{whiteLevel, 0.0f, 0.0f, 0.0f}};

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, static_cast<Uint32>(sizeof(uniforms)));

    SDL_GPUTextureSamplerBinding binding{};
    binding.texture = image;
    binding.sampler = sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

void OverlayRenderer::cleanup() noexcept {
    if (image != nullptr) {
        SDL_ReleaseGPUTexture(device, image);
        image = nullptr;
    }

    if (sampler != nullptr) {
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
    }

    if (pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        pipeline = nullptr;
    }
}
