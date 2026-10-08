#pragma once

#include "math/mat4.h"
#include "render/gpu/gpu_mesh.h"
#include "core/pixel.h"
#include "math/vec2.h"
#include "render/gpu/gpu_texture.h"
#include "scene/material.h"
#include "input/input.h"

#include <string>
#include <map>
#include <utility>
#include <memory>
#include <SDL3/SDL.h>
#include <tuple>
#include <array>
#include <cstddef>
#include <optional>

struct GroundClick {
    // Normalized window coordinates.
    float x;
    float y;
    float aspectRatio;
};

class GpuDisplay {
public:
    GpuDisplay(const char* title, int width, int height);
    ~GpuDisplay();

    // GPU resources must have a single owner.
    GpuDisplay(const GpuDisplay&) = delete;
    GpuDisplay& operator=(const GpuDisplay&) = delete;

    // Returns false when the application should quit.
    bool processEvents(Input& input);
    bool isMouseCaptured() const;

    // Clear colour and depth once. Returns false if no frame is available.
    // Background colour components range from 0.0f to 1.0f.
    bool beginFrame(float red, float green, float blue);

    // Load a material's texture if it is not already cached.
    // Call before beginFrame().
    void prepareMaterial(const Material& material);

    void drawMesh(const GpuMesh& mesh, const Mat4& model, const Mat4& viewProjection, const Material& material);

    // Finish and present the active frame.
    void endFrame();

    SDL_GPUDevice* getDevice() const;

    void toggleFullscreen();

    // Read after beginFrame() returns true.
    float getFrameAspectRatio() const;

    void setMouseLookEnabled(bool enabled);
    bool hasKeyboardFocus() const;
    bool isCursorConfined() const;
    Vec2 getEdgePanDirection(float margin = 5.0f) const;

    std::optional<GroundClick> getGroundClick() const;

private:
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;

    bool videoInitialized = false;
    bool windowClaimed = false;

    void cleanup() noexcept;

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void createPipeline();

    SDL_GPUTexture* depthTexture = nullptr;
    Uint32 depthWidth = 0;
    Uint32 depthHeight = 0;

    void ensureDepthTexture(Uint32 width, Uint32 height);

    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPURenderPass* pass = nullptr;
    
    bool mouseCaptured = false;

    void setMouseCaptured(bool captured);

    // External images use their path.
    // Embedded images use the identity of their shared byte buffer.
    // Both also include the row-flipping setting.
    using TextureKey = std::tuple<std::string, std::shared_ptr<const std::vector<std::uint8_t>>, bool>;

    static TextureKey makeTextureKey(const Material& material) {
        return TextureKey{
            material.texturePath,
            material.embeddedImage,
            material.flipTextureVertically
        };
    }

    std::map<TextureKey, std::unique_ptr<GpuTexture>> textures;
    std::unique_ptr<GpuTexture> whiteTexture;
    void createWhiteTexture();
    bool mouseLookEnabled = true;
    void setCursorConfined(bool confined);
    bool cursorConfined = false;
    std::optional<GroundClick> groundClick;
    bool groundSteeringActive = false;
    void updateGroundSteering();
};