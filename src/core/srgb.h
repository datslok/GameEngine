#pragma once

#include <cstdint>

/*
* Conversions between linear light and sRGB, the encoding image files and colour pickers use.
* sRGB spends more of its values on dark tones, where eyes are most sensitive, roughly encoded = linear^(1/2.2).
* Lighting maths (adding and scaling light) is only correct on linear values, so colours are decoded before shading
* and the swapchain encodes the result again when it is written to the screen.
* All values are 0..1 except bytes.
*/

// Decode an sRGB value to linear light.
float srgbToLinear(float encoded);

// Encode linear light as an sRGB value.
float linearToSrgb(float linear);

// Decode a 0..255 sRGB channel to linear light.
float srgbByteToLinear(std::uint8_t encoded);

// Encode linear light (clamped to 0..1) as the nearest 0..255 sRGB channel.
std::uint8_t linearToSrgbByte(float linear);
