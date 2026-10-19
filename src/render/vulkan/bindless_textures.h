#pragma once

#include "render/vulkan/owned_handle.h"

#include "volk/volk.h"

#include <cstdint>

class VulkanDevice;

/*
* Every texture in one descriptor array ("bindless"): the shader picks a texture by number (a slot), from numbers the
* material carries, instead of each draw binding its own textures. The set is made once and bound once per pass.
*
* A slot is written when its texture is uploaded. Frames already in flight still use the set, which Vulkan normally
* forbids changing; the binding's flags allow it here: update-after-bind (the set may change after being bound) with
* unused-while-pending (only slots no submitted frame reads), and partially bound (slots not yet written are fine as long
* as no shader reads them). Textures are never unloaded, so a slot, once written, never changes.
*/
class BindlessTextures {
public:
    // Room for up to capacity textures, clamped to what the GPU allows.
    BindlessTextures(const VulkanDevice& device, std::uint32_t capacity);

    BindlessTextures(const BindlessTextures&) = delete;
    BindlessTextures& operator=(const BindlessTextures&) = delete;

    VkDescriptorSetLayout getLayout() const;
    VkDescriptorSet getSet() const;
    std::uint32_t getCapacity() const;

    // Point a slot at a texture, read with this sampler. Throws std::out_of_range past the capacity.
    void write(std::uint32_t slot, VkImageView view, VkSampler sampler);

private:
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t capacity = 0;
    OwnedHandle<VkDescriptorSetLayout> layout;
    OwnedHandle<VkDescriptorPool> pool;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
