#include "render/gpu/video_memory.h"

VideoMemory summariseVideoMemory(const std::vector<HeapMemory>& heaps, bool driverReported) {
    VideoMemory memory;
    memory.driverReported = driverReported;

    for (const HeapMemory& heap : heaps) {
        if (heap.deviceLocal) {
            memory.valid = true;
            memory.engineBytes += heap.engineBytes;
            memory.usedBytes += heap.usedBytes;
            memory.budgetBytes += heap.budgetBytes;
        }
    }

    return memory;
}
