#include "engine/debug_views.h"
#include "render/gpu/video_memory.h"

#include <cassert>
#include <vector>

namespace {
    constexpr std::uint64_t megabyte = 1024ull * 1024ull;

    // Video memory is the heaps on the graphics card (device local): system memory the GPU can also reach is left out.
    // A card can have more than one such heap (a small one the CPU can write directly), so they are added up.
    void testOnlyDeviceLocalHeapsCount() {
        const std::vector<HeapMemory> heaps{
            HeapMemory{true, 500 * megabyte, 600 * megabyte, 14000 * megabyte},
            HeapMemory{false, 40 * megabyte, 2000 * megabyte, 30000 * megabyte},
            HeapMemory{true, 20 * megabyte, 30 * megabyte, 200 * megabyte}
        };

        const VideoMemory memory = summariseVideoMemory(heaps, true);

        assert(memory.valid && memory.driverReported);
        assert(memory.engineBytes == 520 * megabyte);
        assert(memory.usedBytes == 630 * megabyte);
        assert(memory.budgetBytes == 14200 * megabyte);
    }

    // With no heaps reported there is nothing to show.
    void testNoHeapsIsNotValid() {
        assert(!summariseVideoMemory({}, true).valid);
        assert(!VideoMemory{}.valid);
    }

    // An integrated GPU has no memory of its own: its one heap is system memory, marked device local, and counts.
    void testIntegratedGpuCounts() {
        const VideoMemory memory = summariseVideoMemory({HeapMemory{true, 300 * megabyte, 900 * megabyte, 8000 * megabyte}}, false);
        assert(memory.valid && !memory.driverReported && memory.engineBytes == 300 * megabyte);
    }

    /*
    * The F4 line: what the whole program uses of the budget the driver gives it, and how much of that the engine
    * allocated itself, in megabytes (2^20 bytes, rounded to the nearest). Without the driver's budget (an old driver
    * without VK_EXT_memory_budget) only the engine's own share is known, and the line says so.
    */
    void testReadoutText() {
        VideoMemory memory;
        memory.valid = true;
        memory.driverReported = true;
        memory.engineBytes = 540 * megabyte + megabyte / 3;
        memory.usedBytes = 612 * megabyte + megabyte * 2 / 3;
        memory.budgetBytes = 14800 * megabyte;
        assert(videoMemoryText(memory) == "VRAM 613 / 14800 MB  ENGINE 540 MB");

        memory.driverReported = false;
        assert(videoMemoryText(memory) == "VRAM ENGINE 540 MB  NO DRIVER BUDGET");

        assert(videoMemoryText(VideoMemory{}) == "VRAM -");
    }

    // The readout's lines: the engine's counts, the light probes, memory, exposure and camera first, then its timings, then the game's own.
    // The camera's line: where it stands in metres and the unit direction it looks, to set up a view somewhere else.
    void testCameraReadout() {
        assert(cameraText(Vec3{-10.04f, 1.7f, 9.0f}, Vec3{-0.981f, -0.05f, 0.2f}) == "CAMERA -10.0 1.7 9.0  LOOK -0.98 -0.05 0.20");
        assert(cameraText(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}) == "CAMERA 0.0 0.0 0.0  LOOK 0.00 0.00 -1.00");
    }

    void testReadoutLinesFollowEachOther() {
        assert(probeReadoutLine == 4);
        assert(videoMemoryReadoutLine == probeReadoutLine + 1);
        assert(exposureReadoutLine == videoMemoryReadoutLine + 1);
        assert(cameraReadoutLine == exposureReadoutLine + 1);
        assert(engineProfileReadoutLine == cameraReadoutLine + 1);
        assert(gpuProfileReadoutLine == engineProfileReadoutLine + 1);
        assert(clothProfileReadoutLine == gpuProfileReadoutLine + 1);
        assert(firstGameReadoutLine == clothProfileReadoutLine + 1);
    }
}

void testVideoMemory() {
    testOnlyDeviceLocalHeapsCount();
    testNoHeapsIsNotValid();
    testIntegratedGpuCounts();
    testReadoutText();
    testReadoutLinesFollowEachOther();
    testCameraReadout();
}
