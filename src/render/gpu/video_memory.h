#pragma once

#include <cstdint>
#include <vector>

/*
* How much video memory the program uses, for the F4 readout. The graphics card's memory comes in heaps: usually one
* big one on the card (device local), a small one of it the CPU can also write, and the system memory the card can
* reach. The memory allocator (VMA) knows what the engine took from each; with the driver's VK_EXT_memory_budget it
* also knows what the whole program uses there and how much the driver will let it use (its budget), which falls when
* other programs need the card too.
*/

// One heap's numbers, as VMA reports them.
struct HeapMemory {
    bool deviceLocal = false;      // memory on the graphics card itself (on an integrated GPU, system memory marked so)
    std::uint64_t engineBytes = 0; // what the engine's allocator took from Vulkan
    std::uint64_t usedBytes = 0;   // what the whole program uses (the engine's share without the driver's numbers)
    std::uint64_t budgetBytes = 0; // how much the driver lets the program use (VMA's estimate without them)
};

// The video memory heaps added up. Not valid until measured.
struct VideoMemory {
    bool valid = false;
    bool driverReported = false; // whether used and budget come from the driver (VK_EXT_memory_budget)
    std::uint64_t engineBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t budgetBytes = 0;
};

// The device local heaps' numbers added up; driverReported says whether the driver gave them.
VideoMemory summariseVideoMemory(const std::vector<HeapMemory>& heaps, bool driverReported);
