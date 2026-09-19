#pragma once

#include <vector>
#include "vec4.h"

bool clipLine(Vec4& start, Vec4& end);

std::vector<Vec4> clipTriangle(
    const Vec4& first,
    const Vec4& second,
    const Vec4& third
);
