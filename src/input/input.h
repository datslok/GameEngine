#pragma once

#include "input/key.h"
#include "math/vec2.h"

#include <array>
#include <cstddef>

/*
* A once-per-frame snapshot of keyboard and mouse input.
* Built from events rather than polled state, so a key that is pressed and released between two frames is still seen.
* Has no SDL dependency: a platform layer (see sdl_input.h) translates its events into these calls.
*/
class Input {
public:
    // Clear the per-frame state. Call once before handling a frame's events.
    void beginFrame();

    // A key went down. Callers must not forward key repeat as new presses.
    void pressKey(Key key);
    void releaseKey(Key key);

    // Add relative mouse motion, in pixels.
    void addMouseMotion(float deltaX, float deltaY);

    // Forget held keys, for example when the window loses focus and key-up events will not arrive.
    void releaseAllKeys();

    // Drop motion collected so far this frame, for example when mouse capture changes.
    void discardMouseMotion();

    bool isKeyHeld(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;

    // Relative mouse motion accumulated over the frame, in pixels.
    Vec2 getMouseDelta() const;

private:
    using KeyStates = std::array<bool, static_cast<std::size_t>(Key::Count)>;

    static bool isValidKey(Key key);

    KeyStates heldKeys{};
    KeyStates pressedKeys{};
    KeyStates releasedKeys{};

    Vec2 mouseDelta{};
};
