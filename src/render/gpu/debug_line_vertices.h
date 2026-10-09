#pragma once

#include "scene/debug_draw.h"

#include <vector>

// One end of a debug line, exactly as the debug line shader reads it (read with offsetof).
struct DebugLineVertex {
    float position[3];
    float colour[3];
};

// Two vertices per line, in order, for drawing as a line list.
std::vector<DebugLineVertex> buildDebugLineVertices(const std::vector<DebugLine>& lines);