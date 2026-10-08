#include "platform/window.h"
#include "platform/sdl_input.h"

#include <stdexcept>
#include <string>

namespace {
    std::runtime_error platformError(const char* message) {
        return std::runtime_error(
            std::string{message} + ": " + SDL_GetError()
        );
    }
}

Window::Window(const char* title, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument(
            "Window dimensions must be positive"
        );
    }

    try {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            throw platformError("SDL initialization failed");
        }

        videoInitialized = true;

        window = SDL_CreateWindow(title, width, height, 0);

        if (window == nullptr) {
            throw platformError("Window creation failed");
        }
    }
    catch (...) {
        cleanup();
        throw;
    }
}

Window::~Window() {
    cleanup();
}

void Window::cleanup() noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    if (videoInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        videoInitialized = false;
    }
}

SDL_Window* Window::getSdlWindow() const {
    return window;
}

/*
* Handle window-level events here and forward keyboard and mouse events to the input snapshot, so game code reads input from one place.
* Presses only count while the window has focus, but releases are always forwarded so nothing can get stuck down.
*/
bool Window::processEvents(Input& input) {
    input.beginFrame();

    const SDL_WindowID windowID = SDL_GetWindowID(window);
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        const bool wasMouseCaptured = mouseCaptured;

        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == windowID) {
            return false;
        }

        // Release the mouse when switching to another window.
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
            event.window.windowID == windowID) {
            setMouseCaptured(false);
            setCursorConfined(false);
            input.releaseAll();
        }

        // Clicking resumes mouse look or confines the visible MOBA cursor.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.windowID == windowID &&
            event.button.button == SDL_BUTTON_LEFT &&
            hasKeyboardFocus()) {
            if (mouseLookEnabled) {
                setMouseCaptured(true);
            } else {
                setCursorConfined(true);
            }
        }

        if ((event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == windowID) ||
            (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == windowID)) {
            if (hasKeyboardFocus()) {
                applySdlEvent(input, event);
            }
        }

        // Always forward releases so keys and buttons cannot get stuck down.
        if ((event.type == SDL_EVENT_KEY_UP && event.key.windowID == windowID) ||
            (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.windowID == windowID)) {
            applySdlEvent(input, event);
        }

        // Alt+Enter toggles fullscreen once per key press.
        if (event.type == SDL_EVENT_KEY_DOWN &&
            event.key.windowID == windowID &&
            !event.key.repeat &&
            event.key.scancode == SDL_SCANCODE_RETURN &&
            (event.key.mod & SDL_KMOD_ALT) != 0) {
            toggleFullscreen();
        }

        // Escape gives the mouse back to the desktop.
        if (event.type == SDL_EVENT_KEY_DOWN &&
            event.key.windowID == windowID &&
            event.key.scancode == SDL_SCANCODE_ESCAPE) {
            setMouseCaptured(false);
            setCursorConfined(false);
        }

        if (event.type == SDL_EVENT_MOUSE_MOTION &&
            event.motion.windowID == windowID &&
            mouseCaptured) {
            applySdlEvent(input, event);
        }

        // Motion from before a capture change would make the camera jump.
        if (mouseCaptured != wasMouseCaptured) {
            input.discardMouseMotion();
        }
    }

    recordCursor(input);

    return true;
}

/*
* Sampled once after the event loop, so game code sees where the cursor is now, for continuous things like edge panning and hold-to-steer.
* Clicks do not use this: they carry their own exact position from the event.
*/
void Window::recordCursor(Input& input) const {
    int width = 0;
    int height = 0;

    if (!SDL_GetWindowSize(window, &width, &height)) {
        throw platformError("Could not get window size");
    }

    input.setWindowSize(Vec2{static_cast<float>(width), static_cast<float>(height)});

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);

    input.setCursor(Vec2{mouseX, mouseY}, SDL_GetMouseFocus() == window);
}

void Window::setMouseCaptured(bool captured) {
    if (mouseCaptured == captured) {
        return;
    }

    if (!SDL_SetWindowRelativeMouseMode(window, captured)) {
        throw platformError("Could not change relative mouse mode");
    }

    mouseCaptured = captured;
}

bool Window::isMouseCaptured() const {
    return mouseCaptured;
}

void Window::toggleFullscreen() {
    const bool fullscreen =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;

    // Use the desktop resolution when entering borderless fullscreen.
    if (!fullscreen) {
        if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
            throw platformError("Could not set borderless fullscreen mode");
        }
    }

    if (!SDL_SetWindowFullscreen(window, !fullscreen)) {
        throw platformError("Could not toggle fullscreen");
    }
}

void Window::setMouseLookEnabled(bool enabled) {
    mouseLookEnabled = enabled;

    const bool focused = hasKeyboardFocus();

    setMouseCaptured(enabled && focused);
    setCursorConfined(!enabled && focused);
}

bool Window::hasKeyboardFocus() const {
    return SDL_GetKeyboardFocus() == window;
}

void Window::setCursorConfined(bool confined) {
    if (!SDL_SetWindowMouseGrab(window, confined)) {
        throw platformError("Could not change cursor confinement");
    }
    cursorConfined = confined;
}

bool Window::isCursorConfined() const {
    return cursorConfined;
}
