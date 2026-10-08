#include "input.h"

#include <SDL3/SDL.h>
#include <cassert>

namespace {
    SDL_Event keyEvent(Uint32 type, SDL_Scancode key, bool repeat = false) {
        SDL_Event event{};
        event.type = type;
        event.key.scancode = key;
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        event.key.repeat = repeat;
        return event;
    }

    SDL_Event motionEvent(float xrel, float yrel) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.xrel = xrel;
        event.motion.yrel = yrel;
        return event;
    }
}

void testInput() {
    Input input;

    // Nothing is held or pressed initially.
    assert(!input.isKeyHeld(SDL_SCANCODE_W));
    assert(!input.wasKeyPressed(SDL_SCANCODE_W));

    // A key-down marks the key as held and pressed.
    input.beginFrame();
    input.handleEvent(keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
    assert(input.isKeyHeld(SDL_SCANCODE_W));
    assert(input.wasKeyPressed(SDL_SCANCODE_W));
    assert(!input.wasKeyReleased(SDL_SCANCODE_W));

    // On the next frame the key stays held but is no longer newly pressed.
    input.beginFrame();
    assert(input.isKeyHeld(SDL_SCANCODE_W));
    assert(!input.wasKeyPressed(SDL_SCANCODE_W));

    // Key repeat does not count as a new press.
    input.handleEvent(keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W, true));
    assert(!input.wasKeyPressed(SDL_SCANCODE_W));

    // A key-up releases the key.
    input.beginFrame();
    input.handleEvent(keyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_W));
    assert(!input.isKeyHeld(SDL_SCANCODE_W));
    assert(input.wasKeyReleased(SDL_SCANCODE_W));

    // A tap within one frame is still seen as pressed and released.
    input.beginFrame();
    input.handleEvent(keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
    input.handleEvent(keyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
    assert(input.wasKeyPressed(SDL_SCANCODE_SPACE));
    assert(input.wasKeyReleased(SDL_SCANCODE_SPACE));
    assert(!input.isKeyHeld(SDL_SCANCODE_SPACE));

    // Mouse motion accumulates over the frame.
    input.beginFrame();
    input.handleEvent(motionEvent(3.0f, -1.0f));
    input.handleEvent(motionEvent(2.0f, 4.0f));
    assert(input.getMouseDelta().x == 5.0f);
    assert(input.getMouseDelta().y == 3.0f);

    // Discarding motion clears it.
    input.discardMouseMotion();
    assert(input.getMouseDelta().x == 0.0f);
    assert(input.getMouseDelta().y == 0.0f);

    // Motion resets at the start of each frame.
    input.handleEvent(motionEvent(1.0f, 1.0f));
    input.beginFrame();
    assert(input.getMouseDelta().x == 0.0f);
    assert(input.getMouseDelta().y == 0.0f);

    // Releasing all keys clears held state.
    input.handleEvent(keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A));
    input.releaseAllKeys();
    assert(!input.isKeyHeld(SDL_SCANCODE_A));

    // Out-of-range scancodes are ignored.
    input.handleEvent(keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_UNKNOWN));
    assert(!input.isKeyHeld(SDL_SCANCODE_UNKNOWN));
    assert(!input.isKeyHeld(SDL_SCANCODE_COUNT));
}
