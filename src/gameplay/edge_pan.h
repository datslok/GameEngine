#pragma once

#include "input/input.h"
#include "math/vec2.h"

/*
* MOBA-style edge panning: which way to move the camera when the cursor is within margin pixels of a window edge.
* x is -1 (left), 0 or +1 (right); y is -1 (top), 0 or +1 (bottom). Zero when the cursor is not over the window.
*/
Vec2 getEdgePanDirection(const Input& input, float margin = 5.0f);
