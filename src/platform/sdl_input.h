#pragma once

#include "input/input.h"
#include "input/key.h"
#include "input/mouse_button.h"

#include <SDL3/SDL.h>
#include <optional>

// Translate SDL's physical key code into an engine key. Keys the engine does not name become Key::Unknown.
Key keyFromScancode(SDL_Scancode scancode);

// Translate an SDL mouse button. Buttons the engine does not name (side buttons) give nothing.
std::optional<MouseButton> mouseButtonFromSdl(Uint8 button);

// Forward one SDL keyboard, mouse button or mouse-motion event to the input snapshot. Other events are ignored.
void applySdlEvent(Input& input, const SDL_Event& event);
