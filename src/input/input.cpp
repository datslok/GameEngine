#include "input/input.h"

#include <cstddef>

/*
* Reset edge-triggered keys and mouse motion so each frame only reports what happened since the previous one.
* Held keys carry over because they stay down until a key-up event arrives.
*/
void Input::beginFrame() {
    pressedKeys.fill(false);
    releasedKeys.fill(false);
    mouseDelta = Vec2{};
}

/*
* Record a key-down. Recording presses separately from held state keeps short taps that start and end within one frame.
*/
void Input::pressKey(Key key) {
    if (!isValidKey(key)) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(key);
    heldKeys[index] = true;
    pressedKeys[index] = true;
}

/*
* Record a key-up, keeping the release visible for this frame even though the key is no longer held.
*/
void Input::releaseKey(Key key) {
    if (!isValidKey(key)) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(key);
    heldKeys[index] = false;
    releasedKeys[index] = true;
}

/*
* Sum every motion event so fast movement is not lost at low frame rates.
*/
void Input::addMouseMotion(float deltaX, float deltaY) {
    mouseDelta.x += deltaX;
    mouseDelta.y += deltaY;
}

/*
* Clear held keys when key-up events can no longer be trusted to arrive, so keys do not stay stuck down.
*/
void Input::releaseAllKeys() {
    heldKeys.fill(false);
}

/*
* Throw away this frame's motion so a capture change does not produce a sudden camera jump.
*/
void Input::discardMouseMotion() {
    mouseDelta = Vec2{};
}

/*
* Report whether the key is currently down, for continuous actions like movement.
*/
bool Input::isKeyHeld(Key key) const {
    return isValidKey(key) && heldKeys[static_cast<std::size_t>(key)];
}

/*
* Report whether the key went down this frame, for one-shot actions like toggles.
*/
bool Input::wasKeyPressed(Key key) const {
    return isValidKey(key) && pressedKeys[static_cast<std::size_t>(key)];
}

/*
* Report whether the key went up this frame.
*/
bool Input::wasKeyReleased(Key key) const {
    return isValidKey(key) && releasedKeys[static_cast<std::size_t>(key)];
}

/*
* Return the total relative motion so mouse look can be applied once per frame.
*/
Vec2 Input::getMouseDelta() const {
    return mouseDelta;
}

/*
* Guard array access against Key::Unknown and values cast from out-of-range integers.
*/
bool Input::isValidKey(Key key) {
    return key > Key::Unknown && key < Key::Count;
}
