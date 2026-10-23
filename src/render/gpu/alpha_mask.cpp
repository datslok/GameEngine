#include "render/gpu/alpha_mask.h"

bool isCutAway(float alpha, float cutoff) {
    return alpha < cutoff;
}

float facingSign(bool frontFacing, bool doubleSided) {
    return doubleSided && !frontFacing ? -1.0f : 1.0f;
}
