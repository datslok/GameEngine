#pragma once

#include "render/gpu/display_output.h"

#include <SDL3/SDL.h>

/*
* What Windows reports about the window's monitor: whether HDR is on, how far above SDR white it can go, and where SDR
* white sits. Read from the window's properties, a cheap lookup, so renderers read it every frame (HDR can be switched
* in Settings, or the window dragged to another screen). Whether the GPU can present a float image to the window is
* the renderer's own question, so hdrSwapchainSupported is left false here.
*/
DisplayCapabilities readWindowDisplay(SDL_Window* window);
