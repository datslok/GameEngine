#pragma once

#include "vec2.h"

#include <SDL3/SDL.h>
#include <array>

/*
* A once-per-frame snapshot of keyboard and mouse input.
* Built from events rather than polled state, so a key that is pressed and released between two frames is still seen.
*/
class Input {
public:
    // Clear the per-frame state. Call once before handling a frame's events.
    void beginFrame();

    void handleEvent(const SDL_Event& event);

    // Forget held keys, for example when the window loses focus and key-up events will not arrive.
    void releaseAllKeys();

    // Drop motion collected so far this frame, for example when mouse capture changes.
    void discardMouseMotion();

    bool isKeyHeld(SDL_Scancode key) const;
    bool wasKeyPressed(SDL_Scancode key) const;
    bool wasKeyReleased(SDL_Scancode key) const;

    // Relative mouse motion accumulated over the frame, in pixels.
    Vec2 getMouseDelta() const;

private:
    using KeyStates = std::array<bool, SDL_SCANCODE_COUNT>;

    static bool isValidKey(SDL_Scancode key);

    KeyStates heldKeys{};
    KeyStates pressedKeys{};
    KeyStates releasedKeys{};

    Vec2 mouseDelta{};
};
