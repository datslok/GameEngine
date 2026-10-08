#pragma once

#include <cstdint>

/*
* Engine-owned key codes, so game code and the simulation do not depend on SDL.
* Keys are physical positions named after a US keyboard: Key::W is the key where W sits there, whatever the layout prints on it, so WASD stays under the left hand on any layout.
*/
enum class Key : std::uint8_t {
    Unknown,

    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    Space, Enter, Escape, Tab, Backspace,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    Up, Down, Left, Right,

    // Number of keys, for sizing arrays. Not a real key.
    Count
};
