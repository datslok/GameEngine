#pragma once

#include "render/present_mode.h"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <span>

/*
* The decisions behind a swapchain (the queue of images that take turns on screen), kept apart from the Vulkan calls so
* they can be tested without a GPU. The renderer asks the surface what it supports, then picks with these.
*/

// SDR: an 8-bit sRGB format (blue-green-red first, red-green-blue as the fallback) in the plain sRGB colour space, so
// the GPU encodes the shader's linear output as it is written. HDR: scRGB, 16-bit floats in the extended linear sRGB
// space (1.0 is SDR white at 80 nits; larger values are brighter). None when the surface offers neither.
std::optional<VkSurfaceFormatKHR> chooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> available, bool hdr);

// The requested mode when the surface offers it, otherwise Vsync (first in, first out), which every driver must offer.
PresentMode choosePresentMode(PresentMode requested, std::span<const VkPresentModeKHR> available);

VkPresentModeKHR vulkanPresentMode(PresentMode mode);

// One image per frame in flight, at least the surface's minimum, at least three for Mailbox (one shown, one waiting,
// one drawn), and at most the surface's maximum (0: none).
std::uint32_t chooseSwapchainImageCount(const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t framesInFlight,
                                        PresentMode mode);

// The surface's own size when it states one, otherwise the window's size in pixels within the surface's limits.
VkExtent2D chooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t windowWidth, std::uint32_t windowHeight);
