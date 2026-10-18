#pragma once

#include "core/build_info.h"
#include "render/gpu/gpu_renderer.h"
#include "render/gpu/render_settings.h"

#include <cstdint>

/*
* The engine's tunable values in one place: the main loop's timing and presentation, and the renderer's settings.
* Application owns one copy; pass your own to its constructor, or change it later through getSettings().
*/
struct EngineSettings {
    // The simulation always advances in steps of exactly 1 / ticksPerSecond. Frames longer than maxFrameSeconds are
    // clamped, so a hitch slows the game down instead of piling up catch-up ticks. Both are read once, at start-up:
    // changing the tick rate of a running simulation would change what it does.
    double ticksPerSecond = 120.0;
    double maxFrameSeconds = 0.25;

    // With mailbox presentation the loop is no longer held back by vsync, so this cap sets the real frame rate.
    // 0 means unlimited. Read every frame.
    std::uint64_t targetFPS = 240;

    // Low latency without tearing. Falls back to vsync on GPUs without mailbox support. Applied whenever it changes.
    PresentMode presentMode = PresentMode::Mailbox;

    // Check every graphics call against the API's rules (SDL's checks, and Vulkan's validation layers when installed).
    // It catches mistakes early but costs a lot of CPU time, so it is on in debug builds and off in release. Read once,
    // at start-up: the GPU device is created with it.
    bool gpuValidation = isDebugBuild;

    RenderSettings render;
};
