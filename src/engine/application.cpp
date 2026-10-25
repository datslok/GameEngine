#include "engine/application.h"
#include "engine/debug_views.h"
#include "engine/golden.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

Application::Application(const char* title, int width, int height, Game& game, const EngineSettings& initialSettings,
                         SavedSettings saved):
    game(game),
    settings(initialSettings),
    defaultSettings(captureDefaultsAndLoad(settings, game, saved)),
    jobs(workerThreadCount(settings)),
    window(title, width, height),
    renderer(window.getSdlWindow(), jobs, settings.gpuValidation, settings.framesInFlight),
    debugUi(window, renderer),
    timestep(settings.ticksPerSecond, settings.maxFrameSeconds)
{
    requestedPresentMode = settings.presentMode;
    renderer.setPresentMode(requestedPresentMode);

    game.useJobs(jobs);
    game.onInit(world, assets);

    mouseLookEnabled = game.wantsMouseLook();
    window.setMouseLookEnabled(mouseLookEnabled);
}

/*
* Run the main loop. Each frame reads input once, lets the game turn it into commands, runs zero or more fixed simulation ticks, then lets the game move the camera and draws between the last two ticks.
*/
void Application::run() {
    Uint64 previousFrameStart = SDL_GetTicksNS();

    while (true) {
        const Uint64 frameStart = SDL_GetTicksNS();
        const double workStart = profilerSeconds();

        if (!window.processEvents(input)) {
            break;
        }

        const double deltaSeconds =
            static_cast<double>(frameStart - previousFrameStart) /
            1'000'000'000.0;

        previousFrameStart = frameStart;
        profiler.record(frameSection, deltaSeconds);

        const int ticks = timestep.advance(deltaSeconds);

        // Before the game reads its settings this frame, so a slider's change shows at once.
        ImDrawData* ui = updateDebugUi();

        game.onInput(world, input);

        for (int tick = 0; tick < ticks; ++tick) {
            ++simulationTicks;
            const double simulationSeconds =
                static_cast<double>(simulationTicks) * timestep.getTickSeconds();

            ScopedTimer timer{profiler, tickSection};
            runFixedUpdate(game, world, static_cast<float>(timestep.getTickSeconds()), simulationSeconds);
        }

        const float alpha = timestep.getAlpha();
        {
            ScopedTimer timer{profiler, updateSection};
            game.onUpdate(world, input, static_cast<float>(timestep.getFrameSeconds()), alpha);
        }

        applyMouseMode();
        applyPresentation();
        render(alpha, static_cast<float>(timestep.getFrameSeconds()), ui);

        const double workEnd = profilerSeconds();
        profiler.record(workSection, workEnd - workStart);
        profiler.update(workEnd);
        gpuProfiler.update(workEnd);

        // 0 means unlimited.
        if (settings.targetFPS > 0) {
            const Uint64 frameDuration =
                1'000'000'000ULL / settings.targetFPS;

            const Uint64 elapsed =
                SDL_GetTicksNS() - frameStart;

            if (elapsed < frameDuration) {
                SDL_DelayNS(frameDuration - elapsed);
            }
        }
    }
}

EngineSettings& Application::getSettings() {
    return settings;
}

SettingsWalk Application::allSettings() {
    return engineAndGameSettings(settings, game);
}

/*
* The game's settings are read the same way, so the game must exist already (it does: it is borrowed). A file that
* cannot be read or holds mistakes never stops the game; each problem is logged and the rest is used. Other games'
* sections of the shared file are left alone (separateSections).
*/
std::vector<SettingEntry> Application::captureDefaultsAndLoad(EngineSettings& settings, Game& game, SavedSettings saved) {
    const SettingsWalk walk = engineAndGameSettings(settings, game);
    std::vector<SettingEntry> defaults = captureSettings(walk);

    if (saved == SavedSettings::Ignore) {
        return defaults;
    }

    try {
        const std::optional<std::string> text = readSettingsFile(savedSettingsPath);

        if (!text) {
            return defaults;
        }

        const ParsedSettings parsed = parseSettingsText(*text);
        const std::vector<SettingEntry> own = separateSections(parsed.entries, walk).own;
        std::vector<std::string> warnings = parsed.warnings;
        const std::vector<std::string> applyWarnings = applySettings(own, walk, StartupFields::Apply);
        warnings.insert(warnings.end(), applyWarnings.begin(), applyWarnings.end());

        const std::string loaded = "Loaded " + std::to_string(own.size()) + " saved settings from " + savedSettingsPath;
        SDL_Log("%s", loaded.c_str());

        for (const std::string& warning : warnings) {
            SDL_Log("%s: %s", savedSettingsPath, warning.c_str());
        }
    }
    catch (const std::exception& error) {
        SDL_Log("Saved settings not loaded: %s", error.what());
    }

    return defaults;
}

/*
* Saving writes only what differs from the defaults, plus the other games' sections as the file holds them now (read
* afresh, so nothing saved since the start is lost). Resetting leaves the start-up fields alone, since the running
* engine was built from them.
*/
ImDrawData* Application::updateDebugUi() {
    if (input.wasKeyPressed(Key::F5)) {
        debugUi.setOpen(!debugUi.isOpen());
    }

    const SettingsWalk walk = allSettings();
    DebugUiRequests requests;
    ImDrawData* ui = debugUi.build(walk, settings.render, requests);

    if (requests.save) {
        try {
            std::vector<SettingEntry> otherGames;
            if (const std::optional<std::string> saved = readSettingsFile(savedSettingsPath)) {
                otherGames = separateSections(parseSettingsText(*saved).entries, walk).others;
            }

            const std::string text = formatSettingsText(captureSettings(walk), defaultSettings, otherGames);
            writeSettingsFile(savedSettingsPath, text);
            const std::size_t changed = parseSettingsText(text).entries.size() - otherGames.size();
            debugUi.setStatus("Saved " + std::to_string(changed) + " changed settings to " + savedSettingsPath);
        }
        catch (const std::exception& error) {
            debugUi.setStatus(std::string{"Not saved: "} + error.what());
        }
    }

    if (requests.resetToDefaults) {
        applySettings(defaultSettings, walk, StartupFields::Keep);
        debugUi.setStatus("Defaults restored (not saved yet)");
    }

    return ui;
}

void Application::applyPresentation() {
    if (settings.presentMode != requestedPresentMode) {
        requestedPresentMode = settings.presentMode;
        renderer.setPresentMode(requestedPresentMode);
    }

    // The renderer clamps the count, and asking again for the count it has costs nothing.
    if (settings.framesInFlight != renderer.getFramesInFlight()) {
        renderer.setFramesInFlight(settings.framesInFlight);
    }
}

void Application::applyMouseMode() {
    // The open debug UI needs the cursor, whatever the game wants.
    const bool wanted = game.wantsMouseLook() && !debugUi.isOpen();

    if (wanted != mouseLookEnabled) {
        mouseLookEnabled = wanted;
        window.setMouseLookEnabled(wanted);
    }
}

/*
* Games may load assets at any time. Assets are only appended, so the new ones are exactly those past the renderer's count: no per-entity checks.
* Uploads must happen before the frame begins.
*/
void Application::prepareNewResources() {
    while (renderer.getMeshCount() < assets.getMeshCount()) {
        const MeshHandle next{static_cast<std::uint32_t>(renderer.getMeshCount())};
        // Convert to the render format once, when the mesh first reaches the GPU; a deforming mesh's rest shape is its
        // topology's, built when it was added.
        if (assets.isDeforming(next)) {
            renderer.uploadMesh(assets.getDeformingTopology(next).rest, true);
        } else {
            renderer.uploadMesh(buildIndexedMesh(assets.getMesh(next)));
        }
        boundsByMesh.push_back(meshBounds(assets.getMesh(next)));
    }

    while (renderer.getTextureCount() < assets.getTextureCount()) {
        const TextureHandle next{static_cast<std::uint32_t>(renderer.getTextureCount())};
        const ImageData& image = assets.getTexture(next);
        renderer.uploadTexture(image.width, image.height, std::span<const Uint8>{image.pixels.data(), image.pixels.size()}, image.encoding, image.mips);
    }
}

/*
* Draw the scene. alpha (0..1) is how far real time has moved past the last tick, and each object is drawn that far between its previous and current transform.
*/
void Application::render(float alpha, float frameSeconds, ImDrawData* ui) {
    prepareNewResources();
    const double sceneStart = profilerSeconds();

    Camera& camera = game.getCamera();
    FrameDescription frame = describeFrame(alpha, frameSeconds);

    // Debug shapes are gathered fresh every frame, so anything not added again disappears.
    debugDraw.clear();

    if (game.wantsDebugView()) {
        drawBoundingBoxes(frame.draws, boundsByMesh, debugDraw);
        drawColliders(world, alpha, debugDraw);

        // From the previous frame: this one has not been drawn yet. At hundreds of frames a second the lag is invisible.
        drawRenderStats(renderer.getLastFrameStats(), debugDraw);
        drawCameraReadout(camera, debugDraw);

        // Also from the previous window: the profiler shows a finished half second at a time.
        drawProfile(profiler, engineProfileReadoutLine, 2, true, profileDebugColour, debugDraw);
        drawProfile(gpuProfiler, gpuProfileReadoutLine, 2, false, profileDebugColour, debugDraw);
    }

    game.onDebugDraw(world, debugDraw, alpha);
    frame.debugLines = debugDraw.getLines();
    frame.debugScreenLines = debugDraw.getScreenLines();

    const double renderStart = profilerSeconds();
    profiler.record(sceneSection, renderStart - sceneStart);
    const bool rendered = renderer.render(frame, settings.render, ui);
    profiler.record(renderSection, profilerSeconds() - renderStart);
    if (!rendered) {
        return;
    }

    recordGpuTimings(gpuProfiler, renderer.getLastFrameStats().gpu);

    // The renderer fitted its copy of the camera to the frame. The game's camera gets the same aspect ratio,
    // so turning clicks into rays (makeCameraRay) matches what is on screen.
    camera.setAspectRatio(renderer.getFrameAspectRatio());
}

FrameDescription Application::describeFrame(float alpha, float frameSeconds) {
    FrameDescription frame = buildFrame(world, game.getCamera(), alpha, &motionHistory);
    frame.lightFocus = game.getLightFocus();

    // Probe events last one frame: this one carries them.
    clearProbeEvents(world);
    frame.frameSeconds = frameSeconds;

    // Each deforming mesh's shape at alpha, one mesh a job: its vertices, and its bounds in place of its rest shape's.
    std::vector<const DeformingMesh*> deforming;
    world.each<DeformingMesh>([&](Entity, DeformingMesh& mesh) { deforming.push_back(&mesh); });
    frame.deformedMeshes.resize(deforming.size());
    jobs.parallelFor(deforming.size(), 1, [&](std::size_t index) {
        frame.deformedMeshes[index] = deformMesh(assets.getDeformingTopology(deforming[index]->mesh), *deforming[index], alpha);
    });
    for (const DeformedMesh& deformed : frame.deformedMeshes) {
        boundsByMesh.at(deformed.mesh.index) = deformed.bounds;
    }
    attachShapes(frame.draws, frame.deformedMeshes);

    // Spheres around each draw let the renderer skip what a view cannot see.
    attachBoundingSpheres(frame.draws, boundsByMesh);
    return frame;
}

/*
* The loop's steps without real time: the game is set up and simulated to the view's time, the assets it loaded are
* uploaded, and the frame is drawn at alpha 1 (on the last tick) off screen at the golden size, after the light probes'
* warm-up frames (each a tick long) if it has probes.
*/
CapturedFrame Application::captureGoldenView(const GoldenView& view, int probeWarmUpFrames) {
    advanceToGoldenView(game, world, view, settings.ticksPerSecond);
    prepareNewResources();

    const float tickSeconds = static_cast<float>(1.0 / static_cast<double>(settings.ticksPerSecond));
    const FrameDescription frame = describeFrame(1.0f, tickSeconds);
    return renderer.capture(frame, settings.render, goldenImageWidth, goldenImageHeight, goldenDisplayOutput(view.hdr), probeWarmUpFrames);
}

std::string Application::getRendererName() const {
    return renderer.getDeviceName();
}
