#pragma once

#include <vector>
#include "vec4.h"

/*
* Clips a line against the view volume so only the visible portion of the line is rendered.
*/
bool clipLine(Vec4& start, Vec4& end);

std::vector<Vec4> clipTriangle(
    const Vec4& first,
    const Vec4& second,
    const Vec4& third
);
