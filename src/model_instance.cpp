#include "model_instance.h"

Mat4 ModelInstance::getMatrix() const {
    return transform.getMatrix() * normalization;
}