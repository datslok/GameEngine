#pragma once
#include <SDL3/SDL.h>

class PixelBuffer;

/*
* Manages the SDL resources required to display pixel data and process window events, keeping ownership of these resources within the class.
* Provides an interface for rendering a PixelBuffer and processing display events.
*/
class Display {
public:
    Display(const char* title, int width, int height);
    ~Display();

    // Prevent copying to ensure SDL resources have a single owner.
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    bool processEvents();
    void present(const PixelBuffer& buffer);

private:
    int width;
    int height;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool videoInitialized = false;

    void cleanup() noexcept;
};