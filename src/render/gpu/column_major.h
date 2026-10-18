#pragma once

#include "math/mat4.h"

/*
* GLSL stores matrices column by column, while Mat4 stores values[row][column], so handing one to a shader is a
* transpose. Writes 16 floats.
*/
inline void writeColumnMajor(const Mat4& matrix, float* target) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            target[column * 4 + row] = matrix.values[row][column];
        }
    }
}
