#pragma once

#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"

#include "volk/volk.h"

#include <cstdint>
#include <span>

/*
* Small helpers for passes that build their own pipeline layouts: the scene's sets kept in place and sets of their own
* after them (the light probes' passes and debug balls, the glass pass).
*/

// A layout of the given sets with one push constant range of pushBytes from offset 0, for pushStages.
OwnedHandle<VkPipelineLayout> createPipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages,
                                                   const char* failure, std::uint32_t pushBytes);

// A set of count bindings (0..count - 1) all of one type.
OwnedHandle<VkDescriptorSetLayout> createUniformLayout(VkDevice device, VkDescriptorType type, std::uint32_t count, VkShaderStageFlags stages,
                                                       const char* failure);

// Images with their samplers into bindings first.. of a set, one each (combined image samplers).
void writeSampledImages(VkDevice device, VkDescriptorSet set, std::uint32_t first, std::span<const SampledImage> images);
