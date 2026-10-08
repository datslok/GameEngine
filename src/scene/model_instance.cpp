#include "scene/model_instance.h"

Mat4 ModelInstance::getMatrix() const {
    return transform.getMatrix() * normalization;
}

Mat4 ModelInstance::getInterpolatedMatrix(float alpha) const {
    return interpolate(previousTransform, transform, alpha).getMatrix() * normalization;
}