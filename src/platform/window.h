#pragma once

#include "input/input.h"

#include <SDL3/SDL.h>

/*
* The operating system window: creation, events, fullscreen, focus and how the mouse is held.
* It reports raw facts into Input (keys, buttons, motion, cursor position, window size). What those facts mean for the game is decided by game code.
*/
class Window {
public:
    Window(const char* title, int width, int height);
    ~Window();

    // The window has a single owner.
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Start a new input frame and fill it from this frame's events. Returns false when the application should quit.
    bool processEvents(Input& input);

    // For the renderer, which draws into this window.
    SDL_Window* getSdlWindow() const;

    void toggleFullscreen();

    // Mouse look hides and captures the cursor; otherwise a visible cursor is confined to the window (MOBA).
    void setMouseLookEnabled(bool enabled);

    // While the debug UI is open, clicks and key presses it uses (the mouse over one of its windows, typing into a field)
    // do not reach Input, so tuning a slider does not also throw a grenade. Events always reach the UI once it has started.
    void setDebugUiOpen(bool open);

    bool isMouseCaptured() const;
    bool isCursorConfined() const;
    bool hasKeyboardFocus() const;

private:
    void cleanup() noexcept;
    void setMouseCaptured(bool captured);
    void setCursorConfined(bool confined);

    // Write the cursor position, window size, focus and mouse state into Input once all events are handled.
    void recordCursor(Input& input) const;

    SDL_Window* window = nullptr;
    bool videoInitialized = false;

    bool mouseLookEnabled = true;
    bool debugUiOpen = false;
    bool mouseCaptured = false;
    bool cursorConfined = false;
};
