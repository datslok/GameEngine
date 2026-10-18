#pragma once

#include "render/gpu/bloom.h"

#include <SDL3/SDL.h>
#include <vector>

/*
* Runs the bloom chain (render/gpu/bloom.h) on the GPU: the lit scene is halved level by level into a chain of small
* float textures, then grown back, each level adding the one below it, so level 0 ends up holding the blurred scene for
* the tone mapping pass to mix in. Owns the level textures (remade when the frame size or level count changes), the
* shrinking and growing pipelines and a linear sampler. Each frame: draw() after the main pass, outside any render pass,
* then bind getResult() in the tone mapping pass.
*/
class BloomRenderer {
public:
    explicit BloomRenderer(SDL_GPUDevice* device);
    ~BloomRenderer();

    // GPU resources must have a single owner.
    BloomRenderer(const BloomRenderer&) = delete;
    BloomRenderer& operator=(const BloomRenderer&) = delete;

    /*
    * Shrinks and grows the scene image (a frame this size), one render pass per step. Returns how the tone mapping pass
    * mixes the result in: strength 0, and nothing drawn, when bloom is off or the frame is too small for a level.
    * Throws std::invalid_argument for nonsense settings.
    */
    BloomComposite draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* scene, Uint32 frameWidth, Uint32 frameHeight,
                        const BloomSettings& settings);

    // The grown level 0 from the last draw(), or null when it drew nothing.
    SDL_GPUTexture* getResult() const;

    // Linear filtering, clamped at the edges: the filters are built around bilinear reads.
    SDL_GPUSampler* getSampler() const;

private:
    // Borrowed device: it must outlive this renderer.
    SDL_GPUDevice* device = nullptr;

    // 11- and 10-bit floats with no alpha: half the memory and bandwidth of 16-bit floats, still far past white, and
    // plenty for a blur. 16-bit floats where a GPU cannot draw into the smaller format.
    SDL_GPUTextureFormat levelFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_GPUGraphicsPipeline* downsamplePipeline = nullptr;
    SDL_GPUGraphicsPipeline* upsamplePipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;

    // One texture per level, level 0 half the frame. Separate textures, not one texture's mip levels, so a pass never
    // reads the texture it draws into.
    std::vector<SDL_GPUTexture*> levels;
    Uint32 levelsFrameWidth = 0;
    Uint32 levelsFrameHeight = 0;
    bool drewLastFrame = false;

    void chooseLevelFormat();
    void createPipelines();
    SDL_GPUGraphicsPipeline* createPipeline(const char* fragmentShaderPath, bool additive);
    void ensureLevels(Uint32 frameWidth, Uint32 frameHeight, std::uint32_t levelCount);
    void releaseLevels() noexcept;
    void runPass(SDL_GPUCommandBuffer* commands, SDL_GPUGraphicsPipeline* pipeline, SDL_GPUTexture* target, bool keepTarget,
                 SDL_GPUTexture* source, const BloomPassUniformData& uniforms);
    void cleanup() noexcept;
};
