#pragma once

#include "render/gpu/cloth_timing.h"
#include "render/vulkan/owned_handle.h"

#include "volk/volk.h"

#include <cstdint>
#include <vector>

class VulkanDevice;

/*
* GPU timestamps between the cloth's passes (render/gpu/cloth_timing.h), one pool per frame in flight: begin writes the
* stamp before the frame's first cloth pass, mark one after each pass with the kind it was, and once the frame's fence
* has passed read gives seconds per kind. Each stamp waits for everything before it (the all-commands stage), which the
* barrier after every cloth pass already makes the GPU do, so stamping adds little. A frame of more passes than a pool
* holds (maxClothPassStamps) is not measured rather than measured in part.
*/
class ClothPassStamps {
public:
    ClothPassStamps(const VulkanDevice& device, std::uint32_t frames);

    // This frame's first stamp, its pool reset (the stamps of the frame that last used it have been read or dropped).
    void begin(VkCommandBuffer commands, std::uint32_t frame);

    // A stamp after a pass of this kind, into the frame begin last started.
    void mark(VkCommandBuffer commands, ClothPass kind);

    // The frame's timings once its fence has passed; not valid if it was not begun since the last read, or overflowed.
    ClothPassTimings read(std::uint32_t frame);

private:
    struct Frame {
        OwnedHandle<VkQueryPool> pool;
        std::vector<ClothPass> kinds;
        bool begun = false;
        bool overflowed = false;
    };

    const VulkanDevice& device;
    std::vector<Frame> frames;
    std::uint32_t current = 0;
};

// The most stamps a frame keeps: about four ticks of three cloths with every level (each tick 1,000 or so passes).
inline constexpr std::uint32_t maxClothPassStamps = 16384;
