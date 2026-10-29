#pragma once

#include "core/settings_visitor.h"
#include "core/build_info.h"
#include "render/frames_in_flight.h"
#include "render/present_mode.h"
#include "render/gpu/render_settings.h"

#include <cstdint>

// EngineSettings::workerThreads for one worker per core but one.
inline constexpr int automaticWorkerThreads = -1;

// The port a game listens for commands on, and --send sends to, unless told otherwise. Below Windows' range for
// short-lived ports (49152 up), so the system does not hand it to another program first.
inline constexpr int defaultCommandPort = 47613;

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

    // The real time a frame may spend on ticks (core/fixed_timestep.h, fitsTickBudget): past it the frame drops the
    // ticks it still owes, so a simulation too slow for real time runs in slow motion at a usable frame rate instead of
    // spiralling down to a few frames a second. 0 for no budget. Read every frame.
    double maxTickSecondsPerFrame = 1.0 / 60.0;

    // With mailbox presentation the loop is no longer held back by vsync, so this cap sets the real frame rate.
    // 0 means unlimited. Read every frame.
    std::uint64_t targetFPS = 120;

    // Low latency without tearing. Falls back to vsync on GPUs without mailbox support. Applied whenever it changes.
    PresentMode presentMode = PresentMode::Mailbox;

    // Frames the CPU may record ahead of the GPU (render/frames_in_flight.h): 2 lets them work at the same time, 1 has
    // the least latency. Applied whenever it changes (the renderer waits for the GPU and rebuilds its per-frame parts).
    std::uint32_t framesInFlight = 2;

    // Check every graphics call against Vulkan's rules (the validation layers, when installed: MSYS2's, see the makefile).
    // It catches mistakes early but costs a lot of CPU time, so it is on in debug builds and off in release. Read once,
    // at start-up: the GPU device is created with it.
    bool gpuValidation = isDebugBuild;

    // The job system's worker threads (core/job_system.h), besides the main thread: automaticWorkerThreads for one per
    // core but one, 0 to run everything on the main thread. Read once, at start-up: the threads are made with the engine.
    int workerThreads = automaticWorkerThreads;

    // Simulate cloth on the GPU (render/vulkan/vulkan_cloth.h), where a game's cloths then cost almost nothing on the
    // CPU, or on the CPU's threads. Read every frame: switching moves each cloth across from its latest points (from the
    // GPU, those a few frames old, so it jumps a little).
    bool clothOnGpu = true;

    // Listen for commands (core/command_registry.h) from tools and AIs on 127.0.0.1 at commandPort, which nothing
    // outside this computer can reach: game.exe --send <command> talks to it. Read once, at start-up.
    bool commandServer = true;
    int commandPort = defaultCommandPort;

    RenderSettings render;
};

// The worker threads the settings ask for.
unsigned workerThreadCount(const EngineSettings& settings);

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(EngineSettings& settings, SettingsVisitor& visitor);
