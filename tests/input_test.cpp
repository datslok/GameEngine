#include "input/input.h"

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

    // Releasing everything clears held keys and buttons.
    input.pressKey(Key::A);
    input.pressMouseButton(MouseButtonPress{MouseButton::Left, Vec2{}, 0});
    input.releaseAll();
    assert(!input.isKeyHeld(Key::A));
    assert(!input.isMouseButtonHeld(MouseButton::Left));

    // Mouse buttons behave like keys: held, pressed and released.
    input.beginFrame();
    input.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{100.0f, 50.0f}, 1000});
    assert(input.isMouseButtonHeld(MouseButton::Right));
    assert(input.wasMouseButtonPressed(MouseButton::Right));
    assert(!input.wasMouseButtonPressed(MouseButton::Left));

    input.beginFrame();
    assert(input.isMouseButtonHeld(MouseButton::Right));
    assert(!input.wasMouseButtonPressed(MouseButton::Right));
    assert(!input.getLastMouseButtonPress(MouseButton::Right));

    input.releaseMouseButton(MouseButton::Right);
    assert(!input.isMouseButtonHeld(MouseButton::Right));
    assert(input.wasMouseButtonReleased(MouseButton::Right));

    // Each press keeps its exact position and time, in order, and the latest press of a button wins.
    input.beginFrame();
    input.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{10.0f, 20.0f}, 5000});
    input.pressMouseButton(MouseButtonPress{MouseButton::Left, Vec2{1.0f, 2.0f}, 6000});
    input.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{30.0f, 40.0f}, 7000});

    assert(input.getMouseButtonPresses().size() == 3);
    assert(input.getMouseButtonPresses()[1].button == MouseButton::Left);

    const auto lastRight = input.getLastMouseButtonPress(MouseButton::Right);
    assert(lastRight);
    assert(lastRight->position.x == 30.0f);
    assert(lastRight->position.y == 40.0f);
    assert(lastRight->timestampNanoseconds == 7000);
    assert(!input.getLastMouseButtonPress(MouseButton::Middle));

    // Presses are cleared at the start of the next frame.
    input.beginFrame();
    assert(input.getMouseButtonPresses().empty());

    // Cursor position and window size are stored as given.
    input.setCursor(Vec2{12.0f, 34.0f}, true);
    input.setWindowSize(Vec2{800.0f, 600.0f});
    assert(input.getCursorPosition().x == 12.0f);
    assert(input.isCursorInWindow());
    assert(input.getWindowSize().y == 600.0f);

    // Out-of-range buttons are ignored.
    input.pressMouseButton(MouseButtonPress{MouseButton::Count, Vec2{}, 0});
    assert(!input.isMouseButtonHeld(MouseButton::Count));
    assert(input.getMouseButtonPresses().empty());

    // Unknown and out-of-range keys are ignored.
    input.pressKey(Key::Unknown);
    input.pressKey(Key::Count);
    assert(!input.isKeyHeld(Key::Unknown));
    assert(!input.isKeyHeld(Key::Count));
}
