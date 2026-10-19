#pragma once

#include "engine/engine_settings.h"
#include "engine/game.h"
#include "games/demo/demo_game.h"
#include "input/input.h"

#include <initializer_list>

/*
* Driving a game headless, shared by the tests that play one (demo_game_test, determinism_test, light_lab_test), so
* each does not grow its own copy.
*/

// The engine's own tick length, so the demo is tested at the rate it runs at.
inline const float tickSeconds = static_cast<float>(1.0 / EngineSettings{}.ticksPerSecond);

// An Input that looks like a focused MOBA window with a confined cursor.
inline Input mobaInput() {
    Input input;
    input.beginFrame();
    input.setWindowSize(Vec2{1600.0f, 900.0f});
    input.setWindowState(true, false, true);
    input.setCursor(Vec2{800.0f, 450.0f}, true);
    return input;
}

// A focused window with the mouse captured, as in first person, holding the given keys.
inline Input firstPersonInput(std::initializer_list<Key> held) {
    Input input;
    input.beginFrame();
    input.setWindowSize(Vec2{1600.0f, 900.0f});
    input.setWindowState(true, true, false);
    for (const Key key : held) {
        input.pressKey(key);
    }
    return input;
}

// Ticks through the engine's own runFixedUpdate, counting the simulation time as the engine does.
inline void runTicks(Game& game, World& world, int count, int& ticksSoFar) {
    for (int i = 0; i < count; ++i) {
        ++ticksSoFar;
        runFixedUpdate(game, world, tickSeconds, ticksSoFar * static_cast<double>(tickSeconds));
    }
}

// One frame of the engine loop for a game: input, the ticks that fall in it, then the per-frame update.
inline void runFrame(Game& game, World& world, const Input& input, int& ticks) {
    game.onInput(world, input);
    runTicks(game, world, 1, ticks);
    game.onUpdate(world, input, tickSeconds, 1.0f);
}
