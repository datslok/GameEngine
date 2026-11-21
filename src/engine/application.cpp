#include "engine/application.h"
#include "assets/texture_cooking.h"
#include "core/background_work.h"
#include "core/build_info.h"
#include "scene/flat_faces.h"
#include "engine/debug_views.h"
#include "engine/golden.h"
#include "engine/gpu_cloth.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/ground_follow.h"
#include "scene/indexed_mesh.h"
#include "scene/level_of_detail.h"
#include "scene/terrain.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

Application::Application(const char* title, int width, int height, Game& game, const EngineSettings& initialSettings,
                         SavedSettings saved, const std::vector<SettingEntry>& launchSettings, WindowVisibility visibility):
    game(game),
    startup(profilerSeconds()),
    settings(initialSettings),
    defaultSettings(captureDefaultsAndLoad(settings, game, saved, launchSettings)),
    jobs(workerThreadCount(settings)),
    window(title, width, height, visibility),
    renderer(window.getSdlWindow(), jobs, settings.gpuValidation, settings.framesInFlight),
    debugUi(window, renderer),
    timestep(settings.ticksPerSecond, settings.maxFrameSeconds)
{
    startup.lap("settings, window and renderer", profilerSeconds());
    requestedPresentMode = settings.presentMode;
    renderer.setPresentMode(requestedPresentMode);

    game.useJobs(jobs);
    workBehindLoadingScreen(loadingWorldTitle, [this] { this->game.onInit(world, assets); });
    startup.lap("world", profilerSeconds());
    uploadMeshesBehindLoadingScreen();
    startup.lap("meshes", profilerSeconds());

    // The renderer's pipelines have been compiling on every core meanwhile; the rest with the loading screen up.
    if (!quitRequested) {
        waitForPipelines();
    }

    startup.lap("shaders", profilerSeconds());

    mouseLookEnabled = game.wantsMouseLook();
    window.setMouseLookEnabled(mouseLookEnabled);

    registerCommands();

    // A port in use (a second game running) leaves this one without commands rather than stopping it.
    if (settings.commandServer) {
        try {
            commandServer.emplace(static_cast<std::uint16_t>(settings.commandPort));
            SDL_Log("Listening for commands on 127.0.0.1:%d (try: game.exe --send help)", settings.commandPort);
        }
        catch (const std::exception& error) {
            SDL_Log("Commands are off: %s", error.what());
        }
    }
}

// A frame of the loading screen at most this often: it only counts, so the threads get the cores. Work that must stay on
// the main thread (meshes going up) gets as long between frames.
constexpr Uint32 loadingFrameMilliseconds = 33;
constexpr double loadingStepSeconds = 0.033;

// A batch of meshes going up together, their clusters built on every thread at once: enough triangles to keep the threads
// busy (about 0.4M a second a thread build them), few enough that a loading frame comes about every 0.2 s on a 16-thread
// laptop; and at most so many meshes, since each upload waits for the GPU on its own.
constexpr std::size_t meshBatchTriangles = 1'000'000;
constexpr std::size_t meshBatchMostMeshes = 256;

// One loading frame, the window shown with the first (Window::reveal), so it never shows black; the sliding block's frame
// counted across every stage, so it moves on smoothly from one to the next.
void Application::drawLoadingFrame(LoadingProgress progress) {
    progress.frame = loadingFrame++;
    renderer.drawLoadingScreen(progress);
    window.reveal();
}

/*
* Work on a thread of its own (core/background_work.h) while this one keeps the loading screen drawn and the window
* answering: the game building its world, and the first frame's scene and sky bake. Safe because a game never touches
* the renderer, the window or SDL (games run headless), the job system takes work from any thread, and nothing else
* reads the world or the assets until it ends; the loading frames touch only the swapchain and their frame slots.
* Closing the window meanwhile cannot stop the work, so it stops the drawing and quits once the work ends.
*/
void Application::workBehindLoadingScreen(std::string_view title, const std::function<void()>& work) {
    runWhileWaiting(work,
                    [this, title] {
                        if (quitRequested) {
                            return;
                        }

                        if (!window.processEvents(input)) {
                            quitRequested = true;
                            return;
                        }

                        drawLoadingFrame(LoadingProgress{.title = title, .counted = false});
                    },
                    std::chrono::milliseconds{loadingFrameMilliseconds});
}

/*
* The world's meshes go up to the GPU before the first frame, counted on the loading screen: a slice of batches between
* frames (stepsWithinBudget), on this thread, since uploads and the screen share the GPU's queue.
*/
void Application::uploadMeshesBehindLoadingScreen() {
    const std::size_t total = assets.getMeshCount();

    while (!quitRequested && renderer.getMeshCount() < total) {
        if (!window.processEvents(input)) {
            quitRequested = true;
            return;
        }

        const std::size_t done = stepsWithinBudget(renderer.getMeshCount(), total, loadingStepSeconds, profilerSeconds, [this](std::size_t) { return uploadNextMeshes(); });
        drawLoadingFrame(LoadingProgress{.title = loadingSceneTitle, .done = done, .total = total});
    }
}

void Application::waitForPipelines() {
    while (!renderer.pipelineProgress().done()) {
        if (!window.processEvents(input)) {
            quitRequested = true;
            return;
        }

        const PipelineProgress progress = renderer.pipelineProgress();
        drawLoadingFrame(LoadingProgress{.title = loadingShadersTitle, .done = progress.built, .total = progress.total});
        SDL_Delay(loadingFrameMilliseconds);
    }

    renderer.finishPipelines();
}

void Application::queueCommands(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        const std::size_t first = line.find_first_not_of(" \t\r");

        if (first != std::string::npos && line[first] != '#') {
            pendingCommands.push_back({std::nullopt, line});
        }
    }
}

/*
* Run the main loop. Each frame reads input once, lets the game turn it into commands, runs zero or more fixed simulation ticks, then lets the game move the camera and draws between the last two ticks.
*/
void Application::run() {
    Uint64 previousFrameStart = SDL_GetTicksNS();

    while (true) {
        // Between frames, before this frame's time is measured: what commands take (a screenshot, many steps) is not
        // game time, so the clock skips it rather than catching up with a burst of ticks.
        const Uint64 commandsStart = SDL_GetTicksNS();
        runCommands();
        previousFrameStart += SDL_GetTicksNS() - commandsStart;

        if (quitRequested) {
            break;
        }

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

        const int ticks = ticksThisFrame(simulation, timestep.advance(deltaSeconds));

        // Before the game reads its settings this frame, so a slider's change shows at once.
        ImDrawData* ui = updateDebugUi();

        // Read every frame, so the debug UI's switch moves the cloths across at the next tick.
        game.useClothSolver(settings.clothOnGpu ? ClothSolver::Gpu : ClothSolver::Cpu);
        game.onInput(world, input);

        // A GPU cloth tick only queues work, so its cost is the GPU's, charged from the last measured frame's.
        const double gpuSecondsPerTick = settings.clothOnGpu ? gpuClothTickEstimate : 0.0;
        const double ticksStart = profilerSeconds();
        for (int tick = 0; tick < ticks; ++tick) {
            if (!fitsTickBudget(tick, profilerSeconds() - ticksStart, gpuSecondsPerTick, settings.maxTickSecondsPerFrame)) {
                break;
            }
            runTick();
        }

        // Paused, the picture rests on the last tick rather than sliding between the last two.
        const float alpha = simulation.paused ? 1.0f : timestep.getAlpha();
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
        clothProfiler.update(workEnd);
        sceneProfiler.update(workEnd);
        renderProfiler.update(workEnd);

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
std::vector<SettingEntry> Application::captureDefaultsAndLoad(EngineSettings& settings, Game& game, SavedSettings saved,
                                                              const std::vector<SettingEntry>& launchSettings) {
    const SettingsWalk walk = engineAndGameSettings(settings, game);
    std::vector<SettingEntry> defaults = captureSettings(walk);

    // Unlike the file's, a mistake on the command line stops the start: whoever typed it is there to fix it.
    const auto applyLaunchSettings = [&] {
        const std::vector<std::string> warnings = applySettings(launchSettings, walk, StartupFields::Apply);
        std::string problems;

        for (const std::string& warning : warnings) {
            problems += "\n--set " + warning;
        }

        if (!problems.empty()) {
            throw std::invalid_argument("The settings given at launch were not all usable:" + problems);
        }
    };

    if (saved == SavedSettings::Ignore) {
        applyLaunchSettings();
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

    applyLaunchSettings();
    return defaults;
}

/*
* The other games' sections are read afresh from the file, so nothing another game saved since the start is lost.
*/
std::size_t Application::saveSettings() {
    const SettingsWalk walk = allSettings();
    std::vector<SettingEntry> otherGames;

    if (const std::optional<std::string> saved = readSettingsFile(savedSettingsPath)) {
        otherGames = separateSections(parseSettingsText(*saved).entries, walk).others;
    }

    const std::string text = formatSettingsText(captureSettings(walk), defaultSettings, otherGames);
    writeSettingsFile(savedSettingsPath, text);
    return parseSettingsText(text).entries.size() - otherGames.size();
}

/*
* Saving writes only what differs from the defaults, plus the other games' sections as the file holds them now
* (saveSettings). Resetting leaves the start-up fields alone, since the running engine was built from them.
*/
ImDrawData* Application::updateDebugUi() {
    if (input.wasKeyPressed(Key::F5)) {
        debugUi.setOpen(!debugUi.isOpen());
    }

    const SettingsWalk walk = allSettings();
    DebugUiRequests requests;
    ImDrawData* ui = debugUi.build(walk, settings.render, typicalGpu, requests);

    if (requests.save) {
        try {
            const std::size_t changed = saveSettings();
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
* Uploads must happen before the frame begins. New textures are compressed (or read from the texture cache) together,
* a texture a job, then uploaded in handle order (docs/research/TEXTURECOMPRESSION.md).
*/
/*
* The next meshes the renderer lacks, a batch (their clusters built on every thread at once: VulkanRenderer::
* uploadMeshes), each converted to the render format once, as it first reaches the GPU; a deforming mesh's rest shape
* is its topology's, built when it was added. A drawn-only mesh is in the render format already, and is never a mirror
* (big rough scenery). Returns how many went up.
*/
std::size_t Application::uploadNextMeshes() {
    const std::size_t first = renderer.getMeshCount();
    const std::size_t total = assets.getMeshCount();
    std::vector<IndexedMesh> converted;
    converted.reserve(meshBatchMostMeshes); // never moved while the batch points into it
    std::vector<VulkanRenderer::MeshUpload> batch;
    std::size_t triangles = 0;
    const auto levelsOf = [this](MeshHandle handle) { return assets.hasClusterLevels(handle) ? ClusterLevels::All : ClusterLevels::One; };

    for (std::size_t index = first; index < total && batch.size() < meshBatchMostMeshes && triangles < meshBatchTriangles; ++index) {
        const MeshHandle handle{static_cast<std::uint32_t>(index)};

        if (assets.isDrawnOnly(handle)) {
            batch.push_back({&assets.getDrawnMesh(handle), false, levelsOf(handle)});
        } else if (assets.isDeforming(handle)) {
            batch.push_back({&assets.getDeformingTopology(handle).rest, true});
        } else {
            converted.push_back(buildIndexedMesh(assets.getMesh(handle)));
            batch.push_back({&converted.back(), false, levelsOf(handle)});
        }

        triangles += batch.back().mesh->indices.size() / 3;
    }

    const std::vector<VertexEntries> entries = renderer.uploadMeshes(batch);

    for (std::size_t within = 0; within < batch.size(); ++within) {
        const MeshHandle handle{static_cast<std::uint32_t>(first + within)};
        vertexEntriesByMesh.push_back(entries[within]);

        if (assets.isDrawnOnly(handle)) {
            boundsByMesh.push_back(indexedMeshBounds(*batch[within].mesh));
            flatFacesByMesh.push_back({});
        } else {
            boundsByMesh.push_back(meshBounds(assets.getMesh(handle)));
            flatFacesByMesh.push_back(assets.isDeforming(handle) ? std::vector<FlatFace>{} : findFlatFaces(assets.getMesh(handle)));
        }
    }

    return batch.size();
}

void Application::prepareNewResources() {
    while (renderer.getMeshCount() < assets.getMeshCount()) {
        uploadNextMeshes();
    }

    const std::size_t firstTexture = renderer.getTextureCount();
    const std::size_t newTextures = assets.getTextureCount() - firstTexture;
    const bool compress = settings.textureCompression.compress && renderer.supportsBlockCompression();
    std::vector<std::optional<CompressedTexture>> cooked(newTextures);
    const double cookStart = profilerSeconds();

    if (compress) {
        jobs.parallelFor(newTextures, 1, [&](std::size_t index) {
            const ImageData& image = assets.getTexture(TextureHandle{static_cast<std::uint32_t>(firstTexture + index)});

            if (shouldCompress(image)) {
                cooked[index] = cookTexture(image, settings.textureCompression, textureCacheFolder, jobs);
            }
        });
    }

    for (std::size_t index = 0; index < newTextures; ++index) {
        if (cooked[index]) {
            renderer.uploadCompressedTexture(*cooked[index]);
            cooked[index].reset();
            continue;
        }

        const ImageData& image = assets.getTexture(TextureHandle{static_cast<std::uint32_t>(firstTexture + index)});
        renderer.uploadTexture(image.width, image.height, std::span<const Uint8>{image.pixels.data(), image.pixels.size()}, image.encoding, image.mips);
    }

    // A first start encodes every texture, which takes a while: say so, with the time, for measuring.
    if (const double seconds = profilerSeconds() - cookStart; compress && seconds > 0.5) {
        SDL_Log("%llu textures compressed or read from %s in %.1f s", static_cast<unsigned long long>(newTextures), textureCacheFolder, seconds);
    }

    if (!startupReported) {
        startup.lap("textures", profilerSeconds());
    }
}

/*
* The first frame builds every still object's draws and bakes the sky visibility round the camera (seconds of CPU in
* the forest): both behind the loading screen, on a thread of its own, exactly as render() would have done them here.
*/
FrameDescription Application::describeFirstFrame(float alpha, float frameSeconds) {
    std::optional<FrameDescription> frame;
    workBehindLoadingScreen(loadingSceneTitle, [&] {
        frame.emplace(describeFrame(alpha, frameSeconds));
        renderer.bakeSkyVisibilityAhead(*frame, settings.render);
    });
    return std::move(*frame);
}

/*
* Draw the scene. alpha (0..1) is how far real time has moved past the last tick, and each object is drawn that far between its previous and current transform.
*/
void Application::render(float alpha, float frameSeconds, ImDrawData* ui) {
    if (!startupReported) {
        startup.lap("first update", profilerSeconds());
    }

    prepareNewResources();
    const double sceneStart = profilerSeconds();

    Camera& camera = game.getCamera();
    FrameDescription frame = startupReported ? describeFrame(alpha, frameSeconds) : describeFirstFrame(alpha, frameSeconds);
    const double debugStart = profilerSeconds();

    if (!startupReported) {
        startup.lap("first scene and sky bake", debugStart);
    }

    // Debug shapes are gathered fresh every frame, so anything not added again disappears.
    debugDraw.clear();

    if (game.wantsDebugView()) {
        // The boxes of what the camera draws, its levels chosen for the height last frame was drawn at, and the colliders:
        // only when asked for (debugBoundingBoxes), since in a forest they hide the readout.
        if (settings.debugBoundingBoxes) {
            const LevelView boxLevels = levelView(camera.getPosition(), static_cast<float>(renderer.getLastFrameStats().outputHeight),
                                                  camera.getVerticalFov(), settings.render.levelOfDetail);
            drawBoundingBoxes(frame.draws, boundsByMesh, camera, boxLevels, debugDraw);
            drawColliders(world, alpha, debugDraw);
        }

        // From the previous frame: this one has not been drawn yet. At hundreds of frames a second the lag is invisible.
        // The readout wraps to the width that frame was presented at.
        debugDraw.setScreenWidth(static_cast<float>(renderer.getLastFrameStats().outputWidth));
        drawRenderStats(renderer.getLastFrameStats(), debugDraw);
        drawCameraReadout(camera, debugDraw);

        // Also from the previous window: the profiler shows a finished half second at a time.
        drawProfile(profiler, 2, true, profileDebugColour, debugDraw);
        drawProfile(sceneProfiler, 2, false, profileDebugColour, debugDraw, true);
        drawProfile(renderProfiler, 2, false, profileDebugColour, debugDraw, true);
        drawGpuProfile(gpuProfiler, debugDraw);

        // The cloth's passes by kind, only while some cloth runs on the GPU and only those kinds that took time; or why
        // they could not be measured.
        if (clothProfiler.getTiming(0).calls > 0 || clothPassesUnmeasured) {
            drawClothReadout(clothProfiler, clothPassesUnmeasured, debugDraw);
        }
    }

    // The wind's arrows round the camera, while the field's debug view is on and the frame has wind.
    windArrowSeconds += frameSeconds;

    if (settings.render.wind.showField && frame.wind) {
        drawWindField(*frame.wind, camera.getPosition(), windArrowSeconds, frame.terrainHeights.get(), debugDraw);
    }

    game.onDebugDraw(world, debugDraw, alpha);
    frame.debugLines = debugDraw.getLines();
    frame.debugScreenLines = debugDraw.getScreenLines();

    const double renderStart = profilerSeconds();
    profiler.record(sceneSection, renderStart - sceneStart);
    sceneProfiler.record(0, renderStart - sceneStart);
    sceneProfiler.record(sceneStageSection(SceneStage::Debug), renderStart - debugStart);
    handOverClothWork(alpha);
    renderer.setClothPassTiming(game.wantsDebugView());
    const bool rendered = renderer.render(frame, settings.render, ui);
    giveBackStillDraws(frame);
    profiler.record(renderSection, profilerSeconds() - renderStart);

    // The start ends with the first frame drawn: one line in the log saying where its time went.
    if (!startupReported) {
        window.reveal(); // in case no loading frame could be drawn
        startup.lap("first frame drawn", profilerSeconds());
        SDL_Log("%s", startup.describe("Start-up").c_str());
        SDL_Log("Mesh clusters and their levels built in %.2f s (part of the start-up)", renderer.getMeshClusterBuildSeconds());

        // The first frame's renderer by stage (render/cpu_timing.h), where its one-off work shows.
        StartupClock stages{0.0};
        const CpuStageTimings& first = renderer.getLastFrameStats().cpu;
        double reached = 0.0;

        for (std::size_t stage = 0; stage < cpuStageCount; ++stage) {
            reached += first.seconds[stage];
            stages.lap(cpuStageName(static_cast<CpuStage>(stage)), reached);
        }

        SDL_Log("%s", stages.describe("First frame's renderer").c_str());
        startupReported = true;
    }

    // What finished frames left of the GPU cloths, for picking and grabbing them.
    applyClothReadbacks(world, renderer.takeClothReadbacks());
    ++frameNumber;
    if (!rendered) {
        return;
    }

    const RenderStats& stats = renderer.getLastFrameStats();
    recordGpuTimings(gpuProfiler, stats.gpu);
    recordClothTimings(clothProfiler, stats.cloth);
    recordCpuStageTimings(renderProfiler, stats.cpu);
    gpuClothTickEstimate = gpuClothTickSeconds(gpuClothTickEstimate, stats.gpu, stats.clothTicks);
    clothPassesUnmeasured = stats.cloth.overflowed;

    // The renderer fitted its copy of the camera to the frame. The game's camera gets the same aspect ratio,
    // so turning clicks into rays (makeCameraRay) matches what is on screen.
    camera.setAspectRatio(renderer.getFrameAspectRatio());
}

void Application::handOverClothWork(float alpha) {
    std::vector<ClothWork> work = takeClothWork(world);
    for (ClothWork& each : work) {
        if (each.mesh.isValid()) {
            each.topology = &assets.getDeformingTopology(each.mesh);
        }
    }
    renderer.setClothWork(std::move(work), alpha);
}

FrameDescription Application::describeFrame(float alpha, float frameSeconds) {
    // Each part's time, for the readout's SCENE line.
    double partStart = profilerSeconds();
    const auto partDone = [&](SceneStage stage) {
        const double now = profilerSeconds();
        sceneProfiler.record(sceneStageSection(stage), now - partStart);
        partStart = now;
    };

    /*
    * The terrain's patches round the camera (scene/terrain.h), chosen first so the frame keeps room for their draws:
    * added to a full list, they moved all of it (35,000 draws, 18 MB, 6 ms a frame). Drawn after the other draws, since
    * their bounds are their own. Its time is the choosing here and the adding below, recorded once.
    */
    validateTerrain(settings.render.terrain);
    std::vector<std::pair<const Terrain*, std::vector<TerrainNode>>> terrains;
    std::size_t terrainDraws = 0;
    world.each<Terrain>([&](Entity, const Terrain& terrain) {
        terrains.emplace_back(&terrain, selectTerrainNodes(terrain, game.getCamera().getPosition(), settings.render.terrain));
        terrainDraws += terrains.back().second.size();
    });
    double terrainSeconds = profilerSeconds() - partStart;
    partStart = profilerSeconds();

    /*
    * Still objects' draws come kept from last frame (scene/still_draws.h), let go when the assets' levels changed, since
    * draws hold spans of them. Debug builds check a few kept matrices a frame against where their entities stand.
    */
    StillDraws* still = nullptr;

    if (settings.keepStillDraws) {
        stillDraws.setCells(settings.cells);

        if (assets.getLevelsVersion() != stillLevelsVersion) {
            stillDraws.invalidate();
            stillLevelsVersion = assets.getLevelsVersion();
        }

        if (isDebugBuild) {
            constexpr std::size_t stillChecksPerFrame = 256;
            stillDraws.verify(world, stillChecksPerFrame);
        }

        still = &stillDraws;
    } else {
        stillDraws.stopLogging(world);
    }

    FrameDescription frame = buildFrame(world, game.getCamera(), alpha, &motionHistory, jobs, terrainDraws, still);
    frame.lightFocus = game.getLightFocus();

    // Probe events and still objects' change marks last one frame: this one carries them.
    clearProbeEvents(world);
    clearRenderChanged(world);
    frame.frameSeconds = frameSeconds;
    partDone(SceneStage::Build);

    /*
    * Each deforming mesh's shape at alpha, one mesh a job: its vertices, and its bounds in place of its rest shape's. A
    * cloth on the GPU is shaped there: its entry has only its bounds, from points as late as the frames in flight and
    * the one being drawn, grown by how far they can have moved.
    */
    std::vector<const DeformingMesh*> deforming;
    std::vector<const Cloth*> onGpu;
    world.each<DeformingMesh>([&](Entity entity, DeformingMesh& mesh) {
        const Cloth* cloth = world.tryGet<Cloth>(entity);
        deforming.push_back(&mesh);
        onGpu.push_back(cloth != nullptr && cloth->onGpu ? cloth : nullptr);
    });
    const float lateSeconds = static_cast<float>(renderer.getFramesInFlight() + 1) * frameSeconds;
    frame.deformedMeshes.resize(deforming.size());
    jobs.parallelFor(deforming.size(), 1, [&](std::size_t index) {
        frame.deformedMeshes[index] = onGpu[index] != nullptr
                                          ? gpuClothDeformedMesh(*onGpu[index], deforming[index]->mesh, lateSeconds, frameNumber)
                                          : deformMesh(assets.getDeformingTopology(deforming[index]->mesh), *deforming[index], alpha);
    });
    for (const DeformedMesh& deformed : frame.deformedMeshes) {
        boundsByMesh.at(deformed.mesh.index) = deformed.bounds;
    }
    // Kept still draws never hold a deforming mesh, so only the draws after them can.
    const std::span<DrawItem> builtDraws = std::span<DrawItem>{frame.draws}.subspan(frame.stillDraws);
    attachShapes(builtDraws, frame.deformedMeshes);
    partDone(SceneStage::Deform);

    // Spheres around each draw let the renderer skip what a view cannot see: attached to the draws built this frame,
    // since kept still draws keep theirs. Those following the ground are lifted onto the land the shaders lift them
    // onto (the last terrain's heights, as the frame's are below); planted parts are not, their spheres being in their
    // plant's frame (the placed table grows them by their layer's relief instead: placed_drawing.h).
    const TerrainHeights* groundHeights = terrains.empty() ? nullptr : terrains.back().first->heights.get();
    const auto attach = [&](std::span<DrawItem> draws, const TerrainHeights* lifting) {
        attachBoundingSpheres(draws, boundsByMesh);
        attachFlatFaces(draws, flatFacesByMesh);
        attachVertexEntries(draws, vertexEntriesByMesh);
        attachMeshLevels(draws, assets.getMeshLevels(), assets.getMeshLevelOptions(), assets.getClusterLevels());
        liftGroundDraws(draws, lifting);
    };

    for (const DrawRange& range : frame.builtStillDraws) {
        attach(std::span<DrawItem>{frame.draws}.subspan(range.first, range.count), groundHeights);
    }

    attach(builtDraws, groundHeights);
    attach(frame.plantedParts, nullptr);
    partDone(SceneStage::Attach);

    for (const auto& [terrain, nodes] : terrains) {
        appendTerrainDraws(*terrain, nodes, frame.draws, frame.terrainPatches);
        frame.terrainHeights = terrain->heights;
        frame.terrainHeightsVersion = terrain->heightsVersion;
    }
    terrainSeconds += profilerSeconds() - partStart;
    sceneProfiler.record(sceneStageSection(SceneStage::Terrain), terrainSeconds);
    partStart = profilerSeconds();

    // The sky visibility grid's level, while its settings want it (each frame's check is a hash of the static draws).
    if (settings.render.skyVisibility.enabled) {
        staticGeometry.update(frame.draws, assets);
        frame.staticLevel = staticGeometry.getLevel();
        frame.staticLevelVersion = staticGeometry.getVersion();
        frame.staticAlbedo = staticGeometry.getAlbedo();
    }

    partDone(SceneStage::Static);
    return frame;
}

// A frame built from kept draws gives them back; one built without them leaves the kept draws let go.
void Application::giveBackStillDraws(FrameDescription& frame) {
    if (settings.keepStillDraws) {
        stillDraws.giveBack(std::move(frame.draws));
    }
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
    FrameDescription frame = describeFrame(1.0f, tickSeconds);
    CapturedFrame captured = renderer.capture(frame, settings.render, goldenImageWidth, goldenImageHeight, goldenDisplayOutput(view.hdr), probeWarmUpFrames);
    giveBackStillDraws(frame);
    return captured;
}

std::string Application::getRendererName() const {
    return renderer.getDeviceName();
}
