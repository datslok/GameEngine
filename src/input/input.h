#pragma once

#include "input/key.h"
#include "input/mouse_button.h"
#include "math/vec2.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

/*
* A once-per-frame snapshot of keyboard and mouse input.
* Built from events rather than polled state, so a key that is pressed and released between two frames is still seen.
* Has no SDL dependency: a platform layer (see platform/window.h and sdl_input.h) translates its events into these calls.
*/
class Input {
public:
    // Clear the per-frame state. Call once before handling a frame's events.
    void beginFrame();

    // A key went down. Callers must not forward key repeat as new presses.
    void pressKey(Key key);
    void releaseKey(Key key);

    void pressMouseButton(const MouseButtonPress& press);
    void releaseMouseButton(MouseButton button);

    // Add relative mouse motion, in pixels.
    void addMouseMotion(float deltaX, float deltaY);

    // Where the cursor is at the end of the frame, in window pixels, and whether it is over the window at all.
    void setCursor(const Vec2& position, bool insideWindow);

    // The window's size in pixels, so game code can turn cursor positions into fractions of the window.
    void setWindowSize(const Vec2& size);

    // How the window holds the keyboard and mouse this frame. Recorded as facts, so games never need the Window itself.
    void setWindowState(bool keyboardFocus, bool mouseCaptured, bool cursorConfined);

    // Forget held keys and buttons, for example when the window loses focus and release events will not arrive.
    void releaseAll();

    // Drop motion collected so far this frame, for example when mouse capture changes.
    void discardMouseMotion();

    bool isKeyHeld(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;

    bool isMouseButtonHeld(MouseButton button) const;
    bool wasMouseButtonPressed(MouseButton button) const;
    bool wasMouseButtonReleased(MouseButton button) const;

    // Every button press this frame, in the order they happened.
    const std::vector<MouseButtonPress>& getMouseButtonPresses() const;

    // The latest press of this button this frame, with its exact position and time.
    std::optional<MouseButtonPress> getLastMouseButtonPress(MouseButton button) const;

    // Relative mouse motion accumulated over the frame, in pixels.
    Vec2 getMouseDelta() const;

    Vec2 getCursorPosition() const;
    bool isCursorInWindow() const;
    Vec2 getWindowSize() const;

    bool hasKeyboardFocus() const;

    // The cursor is hidden and mouse motion drives the camera (first person, free camera).
    bool isMouseCaptured() const;

    // A visible cursor is kept inside the window (MOBA).
    bool isCursorConfined() const;

private:
    using KeyStates = std::array<bool, static_cast<std::size_t>(Key::Count)>;
    using ButtonStates = std::array<bool, static_cast<std::size_t>(MouseButton::Count)>;

    static bool isValidKey(Key key);
    static bool isValidButton(MouseButton button);

    KeyStates heldKeys{};
    KeyStates pressedKeys{};
    KeyStates releasedKeys{};

    ButtonStates heldButtons{};
    ButtonStates pressedButtons{};
    ButtonStates releasedButtons{};
    std::vector<MouseButtonPress> buttonPresses;

    Vec2 mouseDelta{};
    Vec2 cursorPosition{};
    bool cursorInWindow = false;
    Vec2 windowSize{};
    bool keyboardFocus = false;
    bool mouseCaptured = false;
    bool cursorConfined = false;
};
