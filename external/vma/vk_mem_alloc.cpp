// The allocator's implementation, compiled once. Vulkan's functions are found at run time through the
// vkGetInstanceProcAddr and vkGetDeviceProcAddr the renderer passes in (volk loaded them), since VK_NO_PROTOTYPES
// leaves nothing to link against.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"
