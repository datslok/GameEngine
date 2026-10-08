#include "platform/sdl_input.h"

namespace {
    // Offset into a run of keys that is consecutive in both SDL and Key.
    Key offsetKey(Key first, SDL_Scancode scancode, SDL_Scancode firstScancode) {
        return static_cast<Key>(
            static_cast<int>(first) + (static_cast<int>(scancode) - static_cast<int>(firstScancode))
        );
    }
}

/*
* SDL scancodes follow the USB keyboard standard, where A-Z, 1-9 and F1-F12 are consecutive, so those ranges map by offset.
* The digit row runs 1..9 then 0 in that standard, so 0 is handled on its own.
*/
Key keyFromScancode(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return offsetKey(Key::A, scancode, SDL_SCANCODE_A);
    }

    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return offsetKey(Key::Num1, scancode, SDL_SCANCODE_1);
    }

    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return offsetKey(Key::F1, scancode, SDL_SCANCODE_F1);
    }

    switch (scancode) {
    case SDL_SCANCODE_0:         return Key::Num0;
    case SDL_SCANCODE_SPACE:     return Key::Space;
    case SDL_SCANCODE_RETURN:    return Key::Enter;
    case SDL_SCANCODE_ESCAPE:    return Key::Escape;
    case SDL_SCANCODE_TAB:       return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_LSHIFT:    return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:    return Key::RightShift;
    case SDL_SCANCODE_LCTRL:     return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL:     return Key::RightCtrl;
    case SDL_SCANCODE_LALT:      return Key::LeftAlt;
    case SDL_SCANCODE_RALT:      return Key::RightAlt;
    case SDL_SCANCODE_UP:        return Key::Up;
    case SDL_SCANCODE_DOWN:      return Key::Down;
    case SDL_SCANCODE_LEFT:      return Key::Left;
    case SDL_SCANCODE_RIGHT:     return Key::Right;
    default:                     return Key::Unknown;
    }
}

/*
* The one place SDL input events become engine input, so nothing past this point needs SDL.
*/
void applySdlEvent(Input& input, const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        // Key repeat is a text-entry feature, not a new press.
        if (!event.key.repeat) {
            input.pressKey(keyFromScancode(event.key.scancode));
        }
        return;
    }

    if (event.type == SDL_EVENT_KEY_UP) {
        input.releaseKey(keyFromScancode(event.key.scancode));
        return;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        input.addMouseMotion(event.motion.xrel, event.motion.yrel);
    }
}
