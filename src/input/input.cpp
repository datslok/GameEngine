#include "input/input.h"

#include <cstddef>

/*
* Reset edge-triggered keys, buttons and mouse motion so each frame only reports what happened since the previous one.
* Held keys and buttons carry over because they stay down until a release event arrives.
*/
void Input::beginFrame() {
    pressedKeys.fill(false);
    releasedKeys.fill(false);
    pressedButtons.fill(false);
    releasedButtons.fill(false);
    buttonPresses.clear();
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
* Buttons behave like keys, but each press also keeps the exact position and time it happened.
*/
void Input::pressMouseButton(const MouseButtonPress& press) {
    if (!isValidButton(press.button)) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(press.button);
    heldButtons[index] = true;
    pressedButtons[index] = true;
    buttonPresses.push_back(press);
}

void Input::releaseMouseButton(MouseButton button) {
    if (!isValidButton(button)) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(button);
    heldButtons[index] = false;
    releasedButtons[index] = true;
}

/*
* Sum every motion event so fast movement is not lost at low frame rates.
*/
void Input::addMouseMotion(float deltaX, float deltaY) {
    mouseDelta.x += deltaX;
    mouseDelta.y += deltaY;
}

void Input::setCursor(const Vec2& position, bool insideWindow) {
    cursorPosition = position;
    cursorInWindow = insideWindow;
}

void Input::setWindowSize(const Vec2& size) {
    windowSize = size;
}

/*
* Clear held keys and buttons when release events can no longer be trusted to arrive, so nothing stays stuck down.
*/
void Input::releaseAll() {
    heldKeys.fill(false);
    heldButtons.fill(false);
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

bool Input::isMouseButtonHeld(MouseButton button) const {
    return isValidButton(button) && heldButtons[static_cast<std::size_t>(button)];
}

bool Input::wasMouseButtonPressed(MouseButton button) const {
    return isValidButton(button) && pressedButtons[static_cast<std::size_t>(button)];
}

bool Input::wasMouseButtonReleased(MouseButton button) const {
    return isValidButton(button) && releasedButtons[static_cast<std::size_t>(button)];
}

const std::vector<MouseButtonPress>& Input::getMouseButtonPresses() const {
    return buttonPresses;
}

/*
* Search from the end, because when a button is clicked twice in one frame the latest click is the one that counts.
*/
std::optional<MouseButtonPress> Input::getLastMouseButtonPress(MouseButton button) const {
    for (auto press = buttonPresses.rbegin(); press != buttonPresses.rend(); ++press) {
        if (press->button == button) {
            return *press;
        }
    }

    return std::nullopt;
}

/*
* Return the total relative motion so mouse look can be applied once per frame.
*/
Vec2 Input::getMouseDelta() const {
    return mouseDelta;
}

Vec2 Input::getCursorPosition() const {
    return cursorPosition;
}

bool Input::isCursorInWindow() const {
    return cursorInWindow;
}

Vec2 Input::getWindowSize() const {
    return windowSize;
}

/*
* Guard array access against Key::Unknown and values cast from out-of-range integers.
*/
bool Input::isValidKey(Key key) {
    return key > Key::Unknown && key < Key::Count;
}

bool Input::isValidButton(MouseButton button) {
    return button < MouseButton::Count;
}
