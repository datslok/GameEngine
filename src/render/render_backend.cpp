#include "render/render_backend.h"
#include "render/gpu/gpu_renderer.h"
#include "render/vulkan/vulkan_renderer.h"

#include <stdexcept>

std::unique_ptr<RenderBackend> createRenderBackend(GraphicsApi api, SDL_Window* window, bool validation) {
    switch (api) {
    case GraphicsApi::SdlGpu:
        return std::make_unique<GpuRenderer>(window, validation);
    case GraphicsApi::Vulkan:
        return std::make_unique<VulkanRenderer>(window, validation);
    }

    throw std::invalid_argument("Unknown graphics API");
}
