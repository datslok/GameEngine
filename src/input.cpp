#include "input.h"

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
* Record a single event. Recording presses and releases separately from held state keeps short taps that start and end within one frame.
*/
void Input::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        // Key repeat is a text-entry feature, not a new press.
        if (event.key.repeat || !isValidKey(event.key.scancode)) {
            return;
        }

        const std::size_t index = static_cast<std::size_t>(event.key.scancode);
        heldKeys[index] = true;
        pressedKeys[index] = true;
        return;
    }

    if (event.type == SDL_EVENT_KEY_UP) {
        if (!isValidKey(event.key.scancode)) {
            return;
        }

        const std::size_t index = static_cast<std::size_t>(event.key.scancode);
        heldKeys[index] = false;
        releasedKeys[index] = true;
        return;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        // Sum every motion event so fast movement is not lost at low frame rates.
        mouseDelta.x += event.motion.xrel;
        mouseDelta.y += event.motion.yrel;
    }
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
bool Input::isKeyHeld(SDL_Scancode key) const {
    return isValidKey(key) && heldKeys[static_cast<std::size_t>(key)];
}

/*
* Report whether the key went down this frame, for one-shot actions like toggles.
*/
bool Input::wasKeyPressed(SDL_Scancode key) const {
    return isValidKey(key) && pressedKeys[static_cast<std::size_t>(key)];
}

/*
* Report whether the key went up this frame.
*/
bool Input::wasKeyReleased(SDL_Scancode key) const {
    return isValidKey(key) && releasedKeys[static_cast<std::size_t>(key)];
}

/*
* Return the total relative motion so mouse look can be applied once per frame.
*/
Vec2 Input::getMouseDelta() const {
    return mouseDelta;
}

/*
* Guard array access against unknown or out-of-range scancodes.
*/
bool Input::isValidKey(SDL_Scancode key) {
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT;
}
