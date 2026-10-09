#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/debug_draw.h"

#include <cstddef>
#include <string_view>

/*
* Text for debug overlays, drawn with straight strokes like a plotter or an old vector arcade game, so it needs no font
* texture: each character is a few lines on a 4 by 6 grid. It has A-Z (lower case is drawn as upper case), 0-9, space
* and : - / . ? ; anything else shows as ?. Proper font rendering comes later (phase 6); this is for numbers on screen.
*/

// Write text with its top-left corner at a window pixel position. height is the height of a capital letter in pixels.
void debugText(DebugDraw& debug, const Vec2& topLeft, float height, std::string_view text, const Vec3& colour);

// How wide a line of text of that many characters is, in pixels.
float debugTextWidth(float height, std::size_t characters);