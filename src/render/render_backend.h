#pragma once

#include "render/captured_frame.h"
#include "render/gpu/display_output.h"
#include "render/gpu/render_settings.h"
#include "render/present_mode.h"
#include "render/render_stats.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

struct ImDrawData;

/*
* Which graphics API draws the frames, while phase 6b moves the renderer from SDL_GPU to the engine's own Vulkan code.
* Both draw the same frames with the same shaders, so the two can be compared on screen and against the golden images.
* SDL_GPU and this choice go once the Vulkan renderer passes every golden image.
*/
enum class GraphicsApi {
    SdlGpu,
    Vulkan
};

/*
* What the engine asks of a renderer: GpuRenderer (SDL_GPU) and VulkanRenderer both draw a FrameDescription into a
* window they do not own, keep GPU copies of the assets numbered like the AssetManager's handles, and draw the debug UI.
* See GpuRenderer for what each call does; the two behave the same.
*/
class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // Draw and present one frame; false (nothing drawn) when no image was available, such as while minimised.
    virtual bool render(const FrameDescription& frame, const RenderSettings& settings, ImDrawData* ui = nullptr) = 0;

    // The frame drawn off screen at width x height for a fixed display output, and read back (engine/golden.h).
    virtual CapturedFrame capture(const FrameDescription& frame, const RenderSettings& settings, Uint32 width, Uint32 height,
                                  const DisplayOutput& output) = 0;

    // The GPU's name as the driver reports it, for keeping golden images per GPU.
    virtual std::string getDeviceName() const = 0;

    virtual void uploadMesh(const IndexedMesh& mesh) = 0;
    virtual void uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding) = 0;
    virtual std::size_t getMeshCount() const = 0;
    virtual std::size_t getTextureCount() const = 0;

    // The debug UI's drawing half (ImGui's backend for this API). Start it once an ImGui context exists, call
    // beginDebugUiFrame() before each ImGui::NewFrame(), and stop it before the context is destroyed.
    virtual void startDebugUi() = 0;
    virtual void beginDebugUiFrame() = 0;
    virtual void stopDebugUi() = 0;

    // Ask for a present mode; returns the one now in use (Vsync when the requested one is not supported).
    virtual PresentMode setPresentMode(PresentMode requested) = 0;
    virtual PresentMode getPresentMode() const = 0;

    virtual const RenderStats& getLastFrameStats() const = 0;
    virtual const DisplayOutput& getDisplayOutput() const = 0;

    // The aspect ratio of the last frame render() drew.
    virtual float getFrameAspectRatio() const = 0;
};

// The renderer for an API, drawing into window. validation checks every graphics call (slow, for development).
std::unique_ptr<RenderBackend> createRenderBackend(GraphicsApi api, SDL_Window* window, bool validation);
