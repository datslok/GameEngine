#include "render/vulkan/vulkan_check.h"

#include <vulkan/vk_enum_string_helper.h>

#include <string>

std::runtime_error vulkanError(const char* message, VkResult result) {
    return std::runtime_error(std::string{message} + ": " + string_VkResult(result));
}
