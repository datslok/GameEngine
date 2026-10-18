#pragma once

#include "assets/asset_manager.h"
#include "core/fixed_timestep.h"
#include "core/profiler.h"
#include "engine/debug_ui.h"
#include "ecs/world.h"
#include "engine/engine_settings.h"
#include "engine/game.h"
#include "engine/settings_file.h"
#include "input/input.h"
#include "platform/window.h"
#include "engine/golden_view.h"
#include "render/captured_frame.h"
#include "render/gpu/gpu_renderer.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <string>
#include <vector>

// Whether the engine loads the settings the debug UI saved (settings.cfg). Golden image captures ignore them, so every
// capture uses the engine's defaults whatever a player changed.
enum class SavedSettings {
    Load,
    Ignore
};

/*
* The engine: runs the main loop for one Game, and owns everything that is the same for every game (window, renderer, input, World, fixed timestep).
* The game is borrowed, so it must outlive the Application.
* Settings saved by the debug UI (savedSettingsPath) are loaded over the given ones before anything reads them.
*/
class Application {
public:
    Application(const char* title, int width, int height, Game& game, const EngineSettings& initialSettings = {},
                SavedSettings saved = SavedSettings::Load);

    void run();

    // The engine's tunable values. Most are read every frame (see EngineSettings for the ones read only at start-up).
    EngineSettings& getSettings();

    // Bring the game (fresh from onInit, nothing run yet) to a golden view and capture it off screen at the golden size
    // (engine/golden.h). Call it instead of run(), once per Application.
    CapturedFrame captureGoldenView(const GoldenView& view);

    // The GPU's name, for keeping golden images per GPU.
    std::string getRendererName() const;

private:
    // Draw the World from the game's camera, between the last two ticks. ui is the debug UI's frame, or null.
    void render(float alpha, float frameSeconds, ImDrawData* ui);

    // What render() and captures draw: the World seen from the game's camera at alpha, with bounds for culling.
    FrameDescription describeFrame(float alpha, float frameSeconds);

    // Upload meshes and textures the AssetManager gained since the last frame, before the frame begins.
    void prepareNewResources();

    // Tell the window when the game switches between mouse look and a visible cursor.
    void applyMouseMode();

    // Ask the renderer for the settings' present mode when it changes.
    void applyPresentMode();

    // F5 opens and closes the debug UI; while open it is built each frame and its save and reset requests carried out.
    ImDrawData* updateDebugUi();

    // The engine's and the game's settings as one tree, for the debug UI and the settings file.
    SettingsWalk allSettings();

    // Remember the settings as they are (the defaults, for saving only what changed and for resetting), then load the
    // saved file over them. Returns the defaults.
    static std::vector<SettingEntry> captureDefaultsAndLoad(EngineSettings& settings, Game& game, SavedSettings saved);

    Game& game;

    // Before the window and renderer, so the saved settings are loaded before anything is built from them (GPU validation and the
    // timestep are read once). defaultSettings is what they were before loading.
    EngineSettings settings;
    const std::vector<SettingEntry> defaultSettings;

    // Members are destroyed in reverse order: the window outlives the renderer that draws into it,
    // and the renderer owns every GPU copy of the assets.
    Window window;
    GpuRenderer renderer;

    // Refreshed once per frame by window.processEvents().
    Input input;

    World world;

    // CPU copies of every mesh and texture. The renderer keeps GPU copies with the same numbering.
    AssetManager assets;

    // Each uploaded mesh's bounds, indexed like MeshHandle, for the bounding box view.
    std::vector<Aabb> boundsByMesh;

    // After the window and renderer, which it uses until it is destroyed.
    DebugUi debugUi;

    // This frame's wireframe shapes, cleared every frame.
    DebugDraw debugDraw;

    // Built from the settings' tick rate and frame time clamp.
    FixedTimestep timestep;

    // Frame-level timings for the debug readout. Each section is added here, in this order, after the profiler.
    // RENDER includes waiting for the GPU and the swapchain, so it is not all CPU work; WORK is the frame minus the frame cap's sleep.
    Profiler profiler;
    const int frameSection = profiler.addSection("FRAME");
    const int workSection = profiler.addSection("WORK");
    const int tickSection = profiler.addSection("TICK");
    const int updateSection = profiler.addSection("UPDATE");
    const int sceneSection = profiler.addSection("SCENE");
    const int renderSection = profiler.addSection("RENDER");

    // Counting ticks instead of adding up seconds keeps simulation time exact.
    std::uint64_t simulationTicks = 0;

    // The present mode last asked for. Only changes are applied, so a mode the GPU lacks is not asked for every frame.
    PresentMode requestedPresentMode = PresentMode::Vsync;

    // The mouse mode last applied to the window. Only changes are applied, because applying it captures or confines the mouse again, which would undo Escape.
    bool mouseLookEnabled = false;
};
