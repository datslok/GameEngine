// Cut-outs and double-sided surfaces, included with #include "alpha_mask.glsl": mirrors render/gpu/alpha_mask.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef ALPHA_MASK_GLSL
#define ALPHA_MASK_GLSL

// glTF's rule: a texel whose alpha is under the cutoff is a hole.
bool isCutAway(float alpha, float cutoff) {
    return alpha < cutoff;
}

// -1 for the back of a double-sided surface, shaded as the front's mirror image (its whole frame turned round), else 1.
float facingSign(bool frontFacing, bool doubleSided) {
    return doubleSided && !frontFacing ? -1.0 : 1.0;
}

#endif
