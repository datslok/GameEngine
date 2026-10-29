#include "engine/application.h"

#include "assets/image_loader.h"
#include "engine/debug_views.h"
#include "render/gpu/display_output.h"

#include <SDL3/SDL.h>
#include <filesystem>
#include <iostream>
#include <string>

/*
* The Application's commands and how they run (engine/application.h): kept apart from the main loop's file. The
* commands that need no window or GPU are in engine/engine_commands.
*/

namespace {
    // A screenshot's sides: at least a few pixels, and no larger than the GPU's usual image limit.
    constexpr int smallestScreenshotSide = 16;
    constexpr int largestScreenshotSide = 8192;
}

/*
* Exactly the loop's tick: counted, so simulation time stays exact, and timed into the readout's TICK.
*/
void Application::runTick() {
    ++simulationTicks;
    const double simulationSeconds = static_cast<double>(simulationTicks) * timestep.getTickSeconds();

    ScopedTimer timer{profiler, tickSection};
    runFixedUpdate(game, world, static_cast<float>(timestep.getTickSeconds()), simulationSeconds);
}

void Application::registerCommands() {
    addSettingsCommands(commands, allSettings(), defaultSettings);
    addSimulationCommands(commands, simulation, [this] {
        runTick();
    });
    addCameraCommands(commands, game, world);

    commands.add("screenshot", "screenshot <file.png> [width height]",
                 "Saves the picture as the window shows it (the window's size without one given), less the debug UI and lines.",
                 [this](const CommandArguments& arguments) {
                     return screenshot(arguments);
                 });

    commands.add("stats", "stats", "Shows the debug readout (F4) as text: draws, lights, probes, video memory, CPU and GPU times.",
                 [this](const CommandArguments& arguments) {
                     return arguments.empty() ? stats() : commandError("stats takes nothing");
                 });

    commands.add("wait", "wait [frames]",
                 "Lets frames draw (one without a count) before the next command runs, so lighting and anti-aliasing settle.",
                 [this](const CommandArguments& arguments) {
                     if (arguments.size() > 1) {
                         return commandError("Usage: wait [frames]");
                     }

                     const int frames = arguments.empty() ? 1 : intArgument(arguments.front(), "The frames to wait");

                     if (frames < 1 || frames > maxStepTicks) {
                         return commandError("wait takes 1 to " + std::to_string(maxStepTicks) + " frames, not " + std::to_string(frames));
                     }

                     waitFrames = frames;
                     return commandOk("Waited " + std::to_string(frames) + " frames");
                 });

    commands.add("save-settings", "save-settings", "Saves the settings changed from their defaults to settings.cfg, as F5's Save does.",
                 [this](const CommandArguments& arguments) {
                     if (!arguments.empty()) {
                         return commandError("save-settings takes nothing");
                     }

                     const std::size_t changed = saveSettings();
                     return commandOk("Saved " + std::to_string(changed) + " changed settings to " + savedSettingsPath);
                 });

    commands.add("quit", "quit", "Closes the game after replying.", [this](const CommandArguments& arguments) {
        if (!arguments.empty()) {
            return commandError("quit takes nothing");
        }

        quitRequested = true;
        return commandOk("Quitting");
    });
}

/*
* A wait holds back everything after it, so a script's "wait 120" then "screenshot" captures after 120 frames. Its own
* reply is sent when its frames are done, so a client waiting on it knows they are.
*/
void Application::runCommands() {
    if (commandServer) {
        for (CommandRequest& request : commandServer->poll()) {
            pendingCommands.push_back({request.id, std::move(request.line)});
        }
    }

    if (waiting) {
        if (--waitFrames > 0) {
            return;
        }

        answer(waiting->first, waiting->second);
        waiting.reset();
    }

    while (!pendingCommands.empty() && !quitRequested) {
        const PendingCommand command = pendingCommands.front();
        pendingCommands.pop_front();
        const CommandReply reply = commands.execute(command.line);

        if (waitFrames > 0) {
            waiting.emplace(command, reply);
            return;
        }

        answer(command, reply);
    }
}

void Application::answer(const PendingCommand& command, const CommandReply& reply) {
    if (command.connection) {
        if (commandServer) {
            commandServer->reply(*command.connection, encodeReply(reply));
        }
        return;
    }

    std::cout << "> " << command.line << '\n' << (reply.ok ? "" : "ERROR: ") << reply.text << (reply.text.empty() ? "" : "\n") << std::flush;
}

/*
* Drawn again off screen through the golden images' capture, at the window's size and for an ordinary (SDR) screen,
* since a PNG holds SDR. The capture settles temporal anti-aliasing on the same frame first, so the picture is clean,
* and the live view's anti-aliasing then starts afresh (a blur for a frame or two). The frame is the one the window
* would draw next: the game's camera, between the last two ticks.
*/
CommandReply Application::screenshot(const CommandArguments& arguments) {
    if (arguments.size() != 1 && arguments.size() != 3) {
        return commandError("Usage: screenshot <file.png> [width height]");
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window.getSdlWindow(), &width, &height);

    if (arguments.size() == 3) {
        width = intArgument(arguments[1], "The width");
        height = intArgument(arguments[2], "The height");
    }

    if (width < smallestScreenshotSide || height < smallestScreenshotSide || width > largestScreenshotSide || height > largestScreenshotSide) {
        return commandError("A screenshot is " + std::to_string(smallestScreenshotSide) + " to " + std::to_string(largestScreenshotSide) +
                            " pixels a side, not " + std::to_string(width) + " x " + std::to_string(height) + " (is the window minimised?)");
    }

    const std::filesystem::path path{arguments.front()};
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    prepareNewResources();
    const float alpha = simulation.paused ? 1.0f : timestep.getAlpha();
    const FrameDescription frame = describeFrame(alpha, static_cast<float>(timestep.getFrameSeconds()));
    const CapturedFrame captured = renderer.capture(frame, settings.render, static_cast<Uint32>(width), static_cast<Uint32>(height), DisplayOutput{});
    saveRawImagePng(captured.final, path.string());

    return commandOk("Saved " + std::to_string(width) + " x " + std::to_string(height) + " to " + std::filesystem::absolute(path).generic_string());
}

CommandReply Application::stats() {
    std::vector<std::string> lines = renderStatsLines(renderer.getLastFrameStats());
    const Camera& camera = game.getCamera();
    lines.push_back(cameraText(camera.getPosition(), camera.getForward()));
    lines.push_back(profileText(profiler, 2, true));
    lines.push_back(profileText(gpuProfiler, 2, false));

    if (clothProfiler.getTiming(0).calls > 0 || clothPassesUnmeasured) {
        lines.push_back(clothReadoutText(clothProfiler, clothPassesUnmeasured));
    }

    lines.push_back(std::string{simulation.paused ? "PAUSED" : "RUNNING"} + " AT TICK " + std::to_string(simulationTicks));

    std::string text;
    for (const std::string& line : lines) {
        text += (text.empty() ? "" : "\n") + line;
    }

    return commandOk(text);
}
