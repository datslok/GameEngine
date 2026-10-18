#pragma once

#include "scene/debug_draw.h"

#include <vector>

// One end of a debug line, exactly as the debug line shader reads it (read with offsetof).
struct DebugLineVertex {
    float position[3];
    float colour[3];
};

// Two vertices per line, in order, for drawing as a line list. Colours are multiplied by colourScale: what white is
// written as on this screen (1, or an HDR screen's white level), so lines look the same on either.
std::vector<DebugLineVertex> buildDebugLineVertices(const std::vector<DebugLine>& lines, float colourScale = 1.0f);