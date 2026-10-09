#pragma once

#include "assets/asset_manager.h"
#include "core/fixed_timestep.h"
#include "ecs/world.h"
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
    Application(const char* title, int width, int height, Game& game);

    void run();

private:
    // Draw the World from the game's camera, between the last two ticks.
    void render(float alpha);

    // Upload meshes and textures the AssetManager gained since the last frame, before the frame begins.
    void prepareNewResources();

    // Tell the window when the game switches between mouse look and a visible cursor.
    void applyMouseMode();

    // Members are destroyed in reverse order: the window outlives the renderer that draws into it,
    // and the renderer owns every GPU copy of the assets.
    Window window;
    GpuRenderer renderer;

    Game& game;

    // Refreshed once per frame by window.processEvents().
    Input input;

    World world;

    // CPU copies of every mesh and texture. The renderer keeps GPU copies with the same numbering.
    AssetManager assets;

    // Each uploaded mesh's bounds, indexed like MeshHandle, for the bounding box view.
    std::vector<ModelBounds> boundsByMesh;

    // This frame's wireframe shapes, cleared every frame.
    DebugDraw debugDraw;

    // The simulation always advances in steps of exactly 1/120 s.
    // Frames longer than maxFrameSeconds are clamped, so a hitch slows the game down instead of piling up catch-up ticks.
    static constexpr double simulationTicksPerSecond = 120.0;
    static constexpr double maxFrameSeconds = 0.25;
    FixedTimestep timestep{simulationTicksPerSecond, maxFrameSeconds};

    // Counting ticks instead of adding up seconds keeps simulation time exact.
    std::uint64_t simulationTicks = 0;

    // With mailbox presentation the loop is no longer held back by vsync, so this cap sets the real frame rate.
    std::uint64_t targetFPS = 240;

    // The mouse mode last applied to the window. Only changes are applied, because applying it captures or confines the mouse again, which would undo Escape.
    bool mouseLookEnabled = false;
};
