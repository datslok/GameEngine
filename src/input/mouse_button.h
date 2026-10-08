#pragma once

#include "math/vec2.h"

#include <cstdint>

/*
* Engine-owned mouse buttons, so game code does not depend on SDL.
*/
enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,

    // Number of buttons, for sizing arrays. Not a real button.
    Count
};

/*
* One button press, recorded with where and when it happened.
* Using the event's own position and time, rather than the cursor at the end of the frame, keeps clicks exact. The timestamp also allows working out the aim at the moment of a click later (for example for shooting).
*/
struct MouseButtonPress {
    MouseButton button = MouseButton::Left;

    // Window pixels, origin at the top-left.
    Vec2 position{};

    // Nanoseconds on the same clock as SDL_GetTicksNS().
    std::uint64_t timestampNanoseconds = 0;
};
