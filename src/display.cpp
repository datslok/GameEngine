#include "display.h"
#include "pixelbuffer.h"

#include <stdexcept>
#include <string>

/*
* Initalises SDL and creates the window, renderer, and texture required to display pixel data.
* Throws an exception if any of the SDL calls fail, or if the display dimensions are invalid.
*/
Display::Display(const char* title, int width, int height)
    : width(width), height(height)
{
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Display dimensions must be positive");
    }

    // Ensure pixel matches the RGB24 texture format required by SDL.
    static_assert(sizeof(Pixel) == 3,
                  "Pixel must contain exactly 3 bytes");

    try {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            throw std::runtime_error(
                std::string("SDL initialization failed: ") + SDL_GetError()
            );
        }

        videoInitialized = true;

        if (!SDL_CreateWindowAndRenderer(
                title, width, height, 0, &window, &renderer)) {
            throw std::runtime_error(
                std::string("Display creation failed: ") + SDL_GetError()
            );
        }

        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING,
            width,
            height
        );

        if (texture == nullptr) {
            throw std::runtime_error(
                std::string("Texture creation failed: ") + SDL_GetError()
            );
        }
    }
    catch (...) {
        // Clean up any resources created before construction failed as the destructor will not be called for a failed constructor.
        cleanup();
        throw;
    }
}

/*
* Clean up the SDL resources created by the constructor to prevent resource leaks.
*/
Display::~Display() {
    cleanup();
}

/*
* Destroys the SDL resources owned by the display and shut down the video subsystem if it was initialized, ensuring that all resources are released properly.
*/
void Display::cleanup() noexcept {
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }

    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }

    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    if (videoInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        videoInitialized = false;
    }
}

/*
* Process SDL events and return false when the user requests the display to close, allowing the application to terminate cleanly. Otherwise, return true to continue running the application.
*/
bool Display::processEvents() {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window)) {
            return false;
        }
    }

    return true;
}

/*
* Update the SDL texture with the pixel buffer and present it on the display, the buffer dimensions must match the display dimensions to ensure proper rendering. Throws an exception if any of the SDL calls fail.
*/
void Display::present(const PixelBuffer& buffer) {
    if (buffer.getWidth() != static_cast<std::size_t>(width) ||
        buffer.getHeight() != static_cast<std::size_t>(height)) {
        throw std::invalid_argument(
            "Pixel buffer dimensions must match the display"
        );
    }

    // SDL represents row sizes using int. Calculates the number of bytes in each row to match SDL's texture layout.
    const std::size_t rowBytes = buffer.getWidth() * sizeof(Pixel);

    
    if (rowBytes > static_cast<std::size_t>(SDL_MAX_SINT32)) {
        throw std::overflow_error("Pixel row is too large");
    }

    const int pitch = static_cast<int>(rowBytes);

    // Update and render the texture to display the pixel buffer. Throws an exception if any of the SDL calls fail.
    if (!SDL_UpdateTexture(texture, nullptr, buffer.data(), pitch) ||
        !SDL_RenderClear(renderer) ||
        !SDL_RenderTexture(renderer, texture, nullptr, nullptr) ||
        !SDL_RenderPresent(renderer)) {
        throw std::runtime_error(
            std::string("Frame presentation failed: ") + SDL_GetError()
        );
    }
}