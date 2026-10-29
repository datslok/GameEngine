#include "engine/engine_settings.h"

#include "core/job_system.h"

namespace {
    // In the order of PresentMode's values.
    constexpr std::array<const char*, 3> presentModeNames{"Vsync", "Mailbox", "Immediate"};

    static_assert(static_cast<int>(PresentMode::Vsync) == 0 && static_cast<int>(PresentMode::Immediate) == 2,
                  "presentModeNames lists the modes by their values");
}

unsigned workerThreadCount(const EngineSettings& settings) {
    if (settings.workerThreads < 0) {
        return defaultWorkerCount();
    }
    return static_cast<unsigned>(settings.workerThreads);
}

/*
* The tick rate and frame clamp are read once, when the timestep is built, and GPU validation when the device is, so
* they are start-up fields: loaded from the settings file, shown but not changed by the debug UI.
*/
void visit(EngineSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Target FPS", settings.targetFPS, {.min = 0, .max = 10000, .sliderMax = 500, .unit = "(0: unlimited)"});
    visitor.choice("Present mode", settings.presentMode, presentModeNames);
    visitor.field("Frames in flight", settings.framesInFlight, {.min = 1, .max = static_cast<int>(maxFramesInFlight)});
    visitor.field("Ticks per second", settings.ticksPerSecond,
                  {.min = 1.0f, .max = 10000.0f, .sliderMin = 30.0f, .sliderMax = 480.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Max frame seconds", settings.maxFrameSeconds,
                  {.min = 0.001f, .max = 10.0f, .sliderMax = 1.0f, .unit = "s", .applies = FieldApplies::AtStartup});
    visitor.field("Max tick seconds per frame", settings.maxTickSecondsPerFrame,
                  {.min = 0.0f, .max = 1.0f, .sliderMax = 0.1f, .unit = "s (0: no budget)"});
    visitor.field("GPU validation", settings.gpuValidation, FieldApplies::AtStartup);
    visitor.field("Worker threads", settings.workerThreads,
                  {.min = automaticWorkerThreads, .max = 256, .sliderMax = 64, .unit = "(-1: a core each but one)", .applies = FieldApplies::AtStartup});
    visitor.field("Cloth on GPU", settings.clothOnGpu);
    visitor.field("Command server", settings.commandServer, FieldApplies::AtStartup);
    visitor.field("Command port", settings.commandPort,
                  {.min = 1, .max = 65535, .sliderMin = 1024, .sliderMax = 65535, .applies = FieldApplies::AtStartup});
    visitor.group("Render", [&] {
        visit(settings.render, visitor);
    });
}
