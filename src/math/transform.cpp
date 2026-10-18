#include "math/transform.h"

namespace {
    Vec3 lerp(const Vec3& from, const Vec3& to, float alpha) {
        return from + (to - from) * alpha;
    }
}

/*
* Translation times rotation times scale, written out directly: scaling column j of the rotation matrix by scale j and
* putting the position in the last column gives the same matrix as the three products, without the matrix multiplies.
*/
Mat4 Transform::getMatrix() const {
    Mat4 result = rotation.toMatrix();

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.values[row][column] *= scale[column];
        }
        result.values[row][3] = position[row];
    }

    return result;
}

/*
* Used to draw objects between two simulation ticks, so motion looks smooth at any frame rate.
*/
Transform interpolate(const Transform& from, const Transform& to, float alpha) {
    Transform result;

    result.position = lerp(from.position, to.position, alpha);
    result.rotation = slerp(from.rotation, to.rotation, alpha);
    result.scale = lerp(from.scale, to.scale, alpha);

    return result;
}