#include "render/gpu/bloom_renderer.h"
#include "render/gpu/gpu_helpers.h"

#include <stdexcept>
#include <utility>

BloomRenderer::BloomRenderer(SDL_GPUDevice* device):
    device(device)
{
    if (device == nullptr) {
        throw std::invalid_argument("BloomRenderer requires a GPU device");
    }

    try {
        chooseLevelFormat();
        createPipelines();

        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

        sampler = SDL_CreateGPUSampler(device, &samplerInfo);

        if (sampler == nullptr) {
            throw gpuError("Bloom sampler creation failed");
        }
    }
    catch (...) {
        cleanup();
        throw;
    }
}

BloomRenderer::~BloomRenderer() {
    cleanup();
}

void BloomRenderer::chooseLevelFormat() {
    const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    if (SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT, SDL_GPU_TEXTURETYPE_2D, usage)) {
        levelFormat = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    }
    else {
        SDL_Log("Bloom: 11/10-bit float textures not supported, using 16-bit floats");
        levelFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    }
}

/*
* Both steps draw the full-screen triangle into a level: shrinking overwrites the level, growing adds onto it (additive
* blending: the result plus what the level already holds, its own halving).
*/
void BloomRenderer::createPipelines() {
    downsamplePipeline = createPipeline("assets/shaders/bloom_downsample.frag.spv", false);
    upsamplePipeline = createPipeline("assets/shaders/bloom_upsample.frag.spv", true);
}

SDL_GPUGraphicsPipeline* BloomRenderer::createPipeline(const char* fragmentShaderPath, bool additive) {
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;
    SDL_GPUGraphicsPipeline* created = nullptr;

    try {
        vertexShader = loadShader(device, "assets/shaders/fullscreen_triangle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        fragmentShader = loadShader(device, fragmentShaderPath, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

        SDL_GPUColorTargetDescription colourTarget{};
        colourTarget.format = levelFormat;

        if (additive) {
            colourTarget.blend_state.enable_blend = true;
            colourTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colourTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colourTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            colourTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colourTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colourTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }

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

        created = SDL_CreateGPUGraphicsPipeline(device, &info);

        if (created == nullptr) {
            throw gpuError("Bloom pipeline creation failed");
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
    return created;
}

/*
* The levels follow the frame's size and the level count, so they are remade when either changes (a resize, or the
* setting). All new ones are made before the old ones go, so a failure leaves the previous chain intact.
*/
void BloomRenderer::ensureLevels(Uint32 frameWidth, Uint32 frameHeight, std::uint32_t levelCount) {
    if (levels.size() == levelCount && levelsFrameWidth == frameWidth && levelsFrameHeight == frameHeight) {
        return;
    }

    std::vector<SDL_GPUTexture*> replacement;
    replacement.reserve(levelCount);

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const BloomLevelSize size = bloomLevelSize(frameWidth, frameHeight, level);

        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = levelFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = size.width;
        info.height = size.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);

        if (texture == nullptr) {
            const std::runtime_error error = gpuError("Bloom level texture creation failed");

            for (SDL_GPUTexture* made : replacement) {
                SDL_ReleaseGPUTexture(device, made);
            }

            throw error;
        }

        replacement.push_back(texture);
    }

    releaseLevels();
    levels = std::move(replacement);
    levelsFrameWidth = frameWidth;
    levelsFrameHeight = frameHeight;
}

void BloomRenderer::releaseLevels() noexcept {
    for (SDL_GPUTexture* texture : levels) {
        SDL_ReleaseGPUTexture(device, texture);
    }

    levels.clear();
    levelsFrameWidth = 0;
    levelsFrameHeight = 0;
}

/*
* One step: a render pass into one level that runs the full-screen triangle over it, reading the source level. A level
* that is wholly overwritten need not be loaded, and cycling lets the GPU hand out a fresh copy if the previous frame
* still reads it; a level that is added onto must keep what it holds.
*/
void BloomRenderer::runPass(SDL_GPUCommandBuffer* commands, SDL_GPUGraphicsPipeline* pipeline, SDL_GPUTexture* target,
                            bool keepTarget, SDL_GPUTexture* source, const BloomPassUniformData& uniforms) {
    SDL_GPUColorTargetInfo targetInfo{};
    targetInfo.texture = target;
    targetInfo.load_op = keepTarget ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    targetInfo.store_op = SDL_GPU_STOREOP_STORE;
    targetInfo.cycle = !keepTarget;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &targetInfo, 1, nullptr);

    if (pass == nullptr) {
        throw gpuError("Bloom pass creation failed");
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, static_cast<Uint32>(sizeof(uniforms)));

    SDL_GPUTextureSamplerBinding binding{};
    binding.texture = source;
    binding.sampler = sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/*
* Down the chain from the scene (with the Karis average on the first halving only, where single sparkling pixels still
* exist), then back up from the second smallest level, each adding the grown level below it.
*/
BloomComposite BloomRenderer::draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* scene, Uint32 frameWidth, Uint32 frameHeight,
                                   const BloomSettings& settings) {
    drewLastFrame = false;

    const std::uint32_t levelCount = settings.enabled ? bloomLevelCount(frameWidth, frameHeight, settings.maxLevels) : 0;
    const BloomComposite composite = bloomComposite(settings, levelCount);

    if (composite.strength == 0.0f) {
        return composite;
    }

    ensureLevels(frameWidth, frameHeight, levelCount);

    SDL_GPUTexture* source = scene;
    BloomLevelSize sourceSize{frameWidth, frameHeight};

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const BloomLevelSize size = bloomLevelSize(frameWidth, frameHeight, level);
        const float karis = level == 0 && settings.fireflyReduction ? 1.0f : 0.0f;

        runPass(commands, downsamplePipeline, levels[level], false, source, packBloomPass(sourceSize, size, karis));
        source = levels[level];
        sourceSize = size;
    }

    for (std::uint32_t level = levelCount - 1; level > 0; --level) {
        const BloomLevelSize smaller = bloomLevelSize(frameWidth, frameHeight, level);
        const BloomLevelSize target = bloomLevelSize(frameWidth, frameHeight, level - 1);

        runPass(commands, upsamplePipeline, levels[level - 1], true, levels[level], packBloomPass(smaller, target, settings.radius));
    }

    drewLastFrame = true;
    return composite;
}

SDL_GPUTexture* BloomRenderer::getResult() const {
    return drewLastFrame ? levels.front() : nullptr;
}

SDL_GPUSampler* BloomRenderer::getSampler() const {
    return sampler;
}

void BloomRenderer::cleanup() noexcept {
    if (device == nullptr) {
        return;
    }

    releaseLevels();

    if (sampler != nullptr) {
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
    }

    if (downsamplePipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device, downsamplePipeline);
        downsamplePipeline = nullptr;
    }

    if (upsamplePipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device, upsamplePipeline);
        upsamplePipeline = nullptr;
    }
}
