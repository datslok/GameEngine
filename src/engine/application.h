#pragma once

#include "assets/asset_manager.h"
#include "core/command_registry.h"
#include "core/fixed_timestep.h"
#include "core/job_system.h"
#include "core/profiler.h"
#include "engine/debug_views.h"
#include "engine/debug_ui.h"
#include "ecs/world.h"
#include "engine/engine_commands.h"
#include "engine/engine_settings.h"
#include "engine/game.h"
#include "engine/settings_file.h"
#include "input/input.h"
#include "platform/command_socket.h"
#include "platform/window.h"
#include "engine/golden.h"
#include "engine/golden_view.h"
#include "render/captured_frame.h"
#include "render/vulkan/vulkan_renderer.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <deque>
#include <optional>
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
* Settings saved by the debug UI (savedSettingsPath) are loaded over the given ones before anything reads them, then
* launchSettings (--set) over those; a launch setting that is unknown or does not read throws std::invalid_argument.
*
* Commands (core/command_registry.h, engine/engine_commands.h) run at the start of each frame, from a script queued
* before run() and from the command server (EngineSettings::commandServer), one after another in arrival order.
*/
class Application {
public:
    Application(const char* title, int width, int height, Game& game, const EngineSettings& initialSettings = {},
                SavedSettings saved = SavedSettings::Load, const std::vector<SettingEntry>& launchSettings = {});

    void run();

    // Commands to run at the first frame, in order (--run): blank lines and lines starting with # are skipped. Their
    // replies are printed to the standard output.
    void queueCommands(const std::vector<std::string>& lines);

    // The engine's tunable values. Most are read every frame (see EngineSettings for the ones read only at start-up).
    EngineSettings& getSettings();

    // Bring the game (fresh from onInit, nothing run yet) to a golden view and capture it off screen at the golden size
    // (engine/golden.h). Call it instead of run(), once per Application. probeWarmUpFrames: the light probes' frames
    // before the capture; the probes are deterministic, so two captures some frames apart show how much their light
    // flickers between those frames (measuring tools).
    CapturedFrame captureGoldenView(const GoldenView& view, int probeWarmUpFrames = goldenProbeWarmUpFrames);

    // The GPU's name, for keeping golden images per GPU.
    std::string getRendererName() const;

private:
    // Draw the World from the game's camera, between the last two ticks. ui is the debug UI's frame, or null.
    void render(float alpha, float frameSeconds, ImDrawData* ui);

    // What render() and captures draw: the World seen from the game's camera at alpha, with bounds for culling.
    FrameDescription describeFrame(float alpha, float frameSeconds);

    // Upload meshes and textures the AssetManager gained since the last frame, before the frame begins.
    void prepareNewResources();

    // The cloths on the GPU and their queued ticks, handed to the renderer for the frame about to be drawn.
    void handOverClothWork(float alpha);

    // Tell the window when the game switches between mouse look and a visible cursor.
    void applyMouseMode();

    // Ask the renderer for the settings' present mode and frames in flight when they change.
    void applyPresentation();

    // F5 opens and closes the debug UI; while open it is built each frame and its save and reset requests carried out.
    ImDrawData* updateDebugUi();

    // The engine's and the game's settings as one tree, for the debug UI and the settings file.
    SettingsWalk allSettings();

    // Remember the settings as they are (the defaults, for saving only what changed and for resetting), then load the
    // saved file over them and the launch settings over those. Returns the defaults.
    static std::vector<SettingEntry> captureDefaultsAndLoad(EngineSettings& settings, Game& game, SavedSettings saved,
                                                            const std::vector<SettingEntry>& launchSettings);

    // Save what differs from the defaults to savedSettingsPath, keeping other games' sections. Returns how many
    // settings were saved; throws std::runtime_error when the file cannot be written.
    std::size_t saveSettings();

    // One simulation tick, as the loop and the step command run it.
    void runTick();

    // The engine's commands, those of engine/engine_commands.h and the ones needing the window and GPU.
    void registerCommands();

    // Take the server's new commands and run what is queued, unless a wait is still counting down its frames.
    void runCommands();

    // A command's line and who sent it: a connection to reply to, or none for a script (printed).
    struct PendingCommand {
        std::optional<std::uint64_t> connection;
        std::string line;
    };
    void answer(const PendingCommand& command, const CommandReply& reply);

    // The picture as the window shows it (without the debug UI and lines), saved as a PNG.
    CommandReply screenshot(const CommandArguments& arguments);

    // The debug readout's lines (F4) as text, with the simulation's state.
    CommandReply stats();

    Game& game;

    // Before the window and renderer, so the saved settings are loaded before anything is built from them (GPU validation and the
    // timestep are read once). defaultSettings is what they were before loading.
    EngineSettings settings;
    const std::vector<SettingEntry> defaultSettings;

    // Threads for CPU work on every core, before the renderer, which borrows them, and everything else that may.
    JobSystem jobs;

    // Members are destroyed in reverse order: the window outlives the renderer that draws into it,
    // and the renderer owns every GPU copy of the assets.
    Window window;
    VulkanRenderer renderer;

    // Refreshed once per frame by window.processEvents().
    Input input;

    World world;

    // CPU copies of every mesh and texture. The renderer keeps GPU copies with the same numbering.
    AssetManager assets;

    // Each uploaded mesh's bounds, indexed like MeshHandle, for the bounding box view.
    std::vector<Aabb> boundsByMesh;

    // What every model part was drawn with last frame, so each frame's draws say how they moved (for temporal anti-aliasing).
    MotionHistory motionHistory;

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

    // The GPU's time per pass, and the cloth's by kind of pass (while the readout shows), from the renderer's
    // timestamps, averaged over the same window.
    Profiler gpuProfiler = makeGpuProfiler();
    Profiler clothProfiler = makeClothProfiler();

    // Whether the latest cloth timings had more passes than their stamps (the readout says so instead of going blank).
    bool clothPassesUnmeasured = false;

    // What one tick of the GPU cloths costs the GPU, from its timings a frame or two late (gpuClothTickSeconds): the
    // tick budget charges it for every tick while the cloth is on the GPU.
    double gpuClothTickEstimate = 0.0;

    // Counting ticks instead of adding up seconds keeps simulation time exact.
    std::uint64_t simulationTicks = 0;

    // Frames drawn, for GPU cloths' shape fingerprints (engine/gpu_cloth.h).
    std::uint64_t frameNumber = 0;

    // The present mode last asked for. Only changes are applied, so a mode the GPU lacks is not asked for every frame.
    PresentMode requestedPresentMode = PresentMode::Vsync;

    // The mouse mode last applied to the window. Only changes are applied, because applying it captures or confines the mouse again, which would undo Escape.
    bool mouseLookEnabled = false;

    // Every command, and whether the simulation's clock runs (pause, step).
    CommandRegistry commands;
    SimulationControl simulation;

    // Listening while EngineSettings::commandServer is on and its port was free.
    std::optional<CommandServer> commandServer;

    // Commands waiting their turn, and a wait command counting down its frames, its reply held until then.
    std::deque<PendingCommand> pendingCommands;
    int waitFrames = 0;
    std::optional<std::pair<PendingCommand, CommandReply>> waiting;

    // Set by the quit command: the loop ends after the commands' replies are sent.
    bool quitRequested = false;
};
