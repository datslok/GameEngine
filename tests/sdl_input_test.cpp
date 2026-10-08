#include "sdl_input.h"

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

void testSdlInput() {
    // Ends of each consecutive range, to catch off-by-one offsets.
    assert(keyFromScancode(SDL_SCANCODE_A) == Key::A);
    assert(keyFromScancode(SDL_SCANCODE_Z) == Key::Z);
    assert(keyFromScancode(SDL_SCANCODE_W) == Key::W);
    assert(keyFromScancode(SDL_SCANCODE_F1) == Key::F1);
    assert(keyFromScancode(SDL_SCANCODE_F12) == Key::F12);

    // The digit row is 1..9 then 0 in SDL, but 0..9 in Key.
    assert(keyFromScancode(SDL_SCANCODE_1) == Key::Num1);
    assert(keyFromScancode(SDL_SCANCODE_9) == Key::Num9);
    assert(keyFromScancode(SDL_SCANCODE_0) == Key::Num0);

    assert(keyFromScancode(SDL_SCANCODE_SPACE) == Key::Space);
    assert(keyFromScancode(SDL_SCANCODE_LCTRL) == Key::LeftCtrl);
    assert(keyFromScancode(SDL_SCANCODE_RCTRL) == Key::RightCtrl);

    // Keys the engine does not name, and out-of-range values, are unknown.
    assert(keyFromScancode(SDL_SCANCODE_F13) == Key::Unknown);
    assert(keyFromScancode(SDL_SCANCODE_UNKNOWN) == Key::Unknown);
    assert(keyFromScancode(SDL_SCANCODE_COUNT) == Key::Unknown);

    Input input;

    // Key events become engine key presses and releases.
    input.beginFrame();
    applySdlEvent(input, keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
    assert(input.isKeyHeld(Key::W));
    assert(input.wasKeyPressed(Key::W));

    // Key repeat does not count as a new press.
    input.beginFrame();
    applySdlEvent(input, keyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W, true));
    assert(!input.wasKeyPressed(Key::W));

    input.beginFrame();
    applySdlEvent(input, keyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_W));
    assert(!input.isKeyHeld(Key::W));
    assert(input.wasKeyReleased(Key::W));

    // Mouse motion is forwarded and accumulated.
    input.beginFrame();
    applySdlEvent(input, motionEvent(3.0f, -1.0f));
    applySdlEvent(input, motionEvent(2.0f, 4.0f));
    assert(input.getMouseDelta().x == 5.0f);
    assert(input.getMouseDelta().y == 3.0f);
}
