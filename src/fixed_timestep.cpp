#include "fixed_timestep.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

/*
* Store the tick length and the frame time clamp. The clamp must be at least one tick, otherwise a slow frame could never produce a tick.
*/
FixedTimestep::FixedTimestep(double ticksPerSecond, double maxFrameSeconds):
    maxFrameSeconds(maxFrameSeconds)
{
    if (!std::isfinite(ticksPerSecond) || ticksPerSecond <= 0.0) {
        throw std::invalid_argument("Tick rate must be finite and positive");
    }

    tickSeconds = 1.0 / ticksPerSecond;

    if (!std::isfinite(maxFrameSeconds) || maxFrameSeconds < tickSeconds) {
        throw std::invalid_argument("Maximum frame time must be at least one tick");
    }
}

/*
* Deposit real time into the accumulator and withdraw it in whole ticks.
* The clamp stops a long hitch (a breakpoint, dragging the window) from demanding so many catch-up ticks that the game falls further behind every frame.
*/
int FixedTimestep::advance(double seconds) {
    // A clock that jumps backwards or returns garbage must not run the simulation backwards.
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }

    frameSeconds = std::min(seconds, maxFrameSeconds);
    accumulator += frameSeconds;

    int ticks = 0;

    while (accumulator >= tickSeconds) {
        accumulator -= tickSeconds;
        ++ticks;
    }

    return ticks;
}

double FixedTimestep::getTickSeconds() const {
    return tickSeconds;
}

double FixedTimestep::getFrameSeconds() const {
    return frameSeconds;
}

/*
* The time left in the accumulator has not been simulated yet, so rendering blends that far from the previous tick towards the current one.
*/
float FixedTimestep::getAlpha() const {
    return static_cast<float>(accumulator / tickSeconds);
}
