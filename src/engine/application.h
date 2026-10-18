#pragma once

#include "assets/asset_manager.h"
#include "core/fixed_timestep.h"
#include "ecs/world.h"
#include "engine/engine_settings.h"
#include "engine/game.h"
#include "input/input.h"
#include "platform/window.h"
#include "render/gpu/gpu_renderer.h"

#include <cstdint>
#include <vector>

/*
* The engine: runs the main loop for one Game, and owns everything that is the same for every game (window, renderer, input, World, fixed timestep).
* The game is borrowed, so it must outlive the Application.
*/
class Application {
public:
    Application(const char* title, int width, int height, Game& game, const EngineSettings& settings = {});

    void run();

    // The engine's tunable values. Most are read every frame (see EngineSettings for the ones read only at start-up).
    EngineSettings& getSettings();

private:
    // Draw the World from the game's camera, between the last two ticks.
    void render(float alpha, float frameSeconds);

    // Upload meshes and textures the AssetManager gained since the last frame, before the frame begins.
    void prepareNewResources();

    // Tell the window when the game switches between mouse look and a visible cursor.
    void applyMouseMode();

    // Ask the renderer for the settings' present mode when it changes.
    void applyPresentMode();

    // Members are destroyed in reverse order: the window outlives the renderer that draws into it,
    // and the renderer owns every GPU copy of the assets.
    Window window;
    GpuRenderer renderer;

    Game& game;

    // Declared before the timestep, which is built from it.
    EngineSettings settings;

    // Refreshed once per frame by window.processEvents().
    Input input;

    World world;

    // CPU copies of every mesh and texture. The renderer keeps GPU copies with the same numbering.
    AssetManager assets;

    // Each uploaded mesh's bounds, indexed like MeshHandle, for the bounding box view.
    std::vector<Aabb> boundsByMesh;

    // This frame's wireframe shapes, cleared every frame.
    DebugDraw debugDraw;

    // Built from the settings' tick rate and frame time clamp.
    FixedTimestep timestep;

    // Counting ticks instead of adding up seconds keeps simulation time exact.
    std::uint64_t simulationTicks = 0;

    // The present mode last asked for. Only changes are applied, so a mode the GPU lacks is not asked for every frame.
    PresentMode requestedPresentMode = PresentMode::Vsync;

    // The mouse mode last applied to the window. Only changes are applied, because applying it captures or confines the mouse again, which would undo Escape.
    bool mouseLookEnabled = false;
};
