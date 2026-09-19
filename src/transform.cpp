#include "transform.h"

Mat4 Transform::getMatrix() const {
    return
        Mat4::translation(position.x, position.y, position.z) *
        Mat4::rotationZ(rotation.z) *
        Mat4::rotationY(rotation.y) *
        Mat4::rotationX(rotation.x) *
        Mat4::scaling(scale.x, scale.y, scale.z);
}