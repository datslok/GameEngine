#pragma once

#include "input/input.h"
#include "input/key.h"

#include <SDL3/SDL.h>

// Translate SDL's physical key code into an engine key. Keys the engine does not name become Key::Unknown.
Key keyFromScancode(SDL_Scancode scancode);

// Forward one SDL keyboard or mouse-motion event to the input snapshot. Other events are ignored.
void applySdlEvent(Input& input, const SDL_Event& event);
