#include "input.h"

#include <cassert>

// No SDL here: Input is driven entirely by engine types.
void testInput() {
    Input input;

    // Nothing is held or pressed initially.
    assert(!input.isKeyHeld(Key::W));
    assert(!input.wasKeyPressed(Key::W));

    // A key-down marks the key as held and pressed.
    input.beginFrame();
    input.pressKey(Key::W);
    assert(input.isKeyHeld(Key::W));
    assert(input.wasKeyPressed(Key::W));
    assert(!input.wasKeyReleased(Key::W));

    // On the next frame the key stays held but is no longer newly pressed.
    input.beginFrame();
    assert(input.isKeyHeld(Key::W));
    assert(!input.wasKeyPressed(Key::W));

    // A key-up releases the key.
    input.beginFrame();
    input.releaseKey(Key::W);
    assert(!input.isKeyHeld(Key::W));
    assert(input.wasKeyReleased(Key::W));

    // A tap within one frame is still seen as pressed and released.
    input.beginFrame();
    input.pressKey(Key::Space);
    input.releaseKey(Key::Space);
    assert(input.wasKeyPressed(Key::Space));
    assert(input.wasKeyReleased(Key::Space));
    assert(!input.isKeyHeld(Key::Space));

    // Mouse motion accumulates over the frame.
    input.beginFrame();
    input.addMouseMotion(3.0f, -1.0f);
    input.addMouseMotion(2.0f, 4.0f);
    assert(input.getMouseDelta().x == 5.0f);
    assert(input.getMouseDelta().y == 3.0f);

    // Discarding motion clears it.
    input.discardMouseMotion();
    assert(input.getMouseDelta().x == 0.0f);
    assert(input.getMouseDelta().y == 0.0f);

    // Motion resets at the start of each frame.
    input.addMouseMotion(1.0f, 1.0f);
    input.beginFrame();
    assert(input.getMouseDelta().x == 0.0f);
    assert(input.getMouseDelta().y == 0.0f);

    // Releasing all keys clears held state.
    input.pressKey(Key::A);
    input.releaseAllKeys();
    assert(!input.isKeyHeld(Key::A));

    // Unknown and out-of-range keys are ignored.
    input.pressKey(Key::Unknown);
    input.pressKey(Key::Count);
    assert(!input.isKeyHeld(Key::Unknown));
    assert(!input.isKeyHeld(Key::Count));
}
