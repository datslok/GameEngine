#pragma once

#include <array>
#include <cstddef>

/*
* The renderer's CPU time by stage, for the F4 readout's RENDER line and the stats command: where a frame's CPU work goes
* before the GPU sees it, so a slow frame can be traced to its cause (RENDER also holds the waits for the GPU and the
* swapchain, which WAIT and SUBMIT show apart).
*/
enum class CpuStage {
    SkyBake,    // the sky visibility grid's CPU bake before the frame begins
    Wait,       // waiting for the frame in flight's GPU work and the swapchain's image
    Setup,      // the rest: the cloth, the camera, lights ranked, probes, fog, the frame's images
    Camera,     // what the camera draws: culled, levels chosen, batched (and the velocity pass's)
    Lights,     // lights packed, the mirror chosen, the shadow tiles planned
    Shadows,    // each shadow tile's casters: culled, levels chosen, batched
    Captures,   // the reflection probe's faces and the flat mirror's view: culled, levels chosen, batched
    Lines,      // the debug lines written into the frame's upload memory
    Pack,       // the scene's buffers written: instances, materials, lights, draw lists
    Record,     // the passes' commands recorded
    Submit      // the commands submitted and the image presented
};

inline constexpr std::size_t cpuStageCount = static_cast<std::size_t>(CpuStage::Submit) + 1;

// The stage's name on the readout, short and upper case.
const char* cpuStageName(CpuStage stage);

// One frame's CPU time by stage, in seconds, and their sum; not valid when the frame was not measured.
struct CpuStageTimings {
    std::array<double, cpuStageCount> seconds{};
    double totalSeconds = 0.0;
    bool valid = false;
};

/*
* Times a frame's stages from the clock readings it is given (profilerSeconds()): start at the frame's beginning, then
* each lap gives the time since the last reading to that stage, so a stage met twice adds up. finish hands the frame's
* timings over and stops; laps before the next start count nothing.
*/
class CpuStageClock {
public:
    void start(double nowSeconds);
    void lap(CpuStage stage, double nowSeconds);
    CpuStageTimings finish();

private:
    CpuStageTimings timings;
    double lastSeconds = 0.0;
    bool running = false;
};
