#pragma once
#include <SDL3/SDL.h>

class PixelBuffer;

class Display {
public:
    Display(const char* title, int width, int height);
    ~Display();

    // Prevent two objects from owning the same SDL resources.
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