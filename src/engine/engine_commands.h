#pragma once

#include "core/command_registry.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "engine/settings_file.h"

#include <functional>
#include <vector>

/*
* The engine's commands that need no window or GPU (core/command_registry.h), so the tests run them headless. The
* Application adds them with its own (screenshot, stats, wait, quit, save-settings) and runs them between frames.
*/

/*
* set, get, list and reset over a tree of settings (the engine's and the game's, engineAndGameSettings), by their keys
* in the settings file. They act on the live values, which the debug UI (F5) draws from, so a change shows at once.
* defaults are the values reset returns to. Fields read only at start-up are refused by set and left alone by reset.
*/
void addSettingsCommands(CommandRegistry& registry, SettingsWalk walk, std::vector<SettingEntry> defaults);

// Whether the simulation's clock runs. Paused, frames still draw and the camera still moves, but no tick runs.
struct SimulationControl {
    bool paused = false;
};

// The ticks a frame runs: those due, or none while paused (their time is dropped, so resuming does not race to catch up).
int ticksThisFrame(const SimulationControl& control, int ticksDue);

// The most ticks one step command runs: 100,000 ticks is about 14 minutes of game time at 120 a second.
inline constexpr int maxStepTicks = 100000;

/*
* pause, resume and step: step pauses, then runs its ticks at once through runTick, which must run one tick exactly as
* the main loop does, so a stepped game reaches the very state of one that ran.
*/
void addSimulationCommands(CommandRegistry& registry, SimulationControl& control, std::function<void()> runTick);

// camera: shows the camera's place and direction, or places it (Game::poseCamera), looking at a point or as before.
void addCameraCommands(CommandRegistry& registry, Game& game, World& world);
