#pragma once

#include "volk/volk.h"

#include <stdexcept>

// An exception whose message is ours followed by the name of Vulkan's result code (VK_ERROR_OUT_OF_DEVICE_MEMORY...).
std::runtime_error vulkanError(const char* message, VkResult result);

// Vulkan reports failure through its return value; anything but VK_SUCCESS becomes an exception.
inline void vulkanCheck(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw vulkanError(message, result);
    }
}
