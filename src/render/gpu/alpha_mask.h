#pragma once

/*
* Cut-outs and double-sided surfaces, mirrored from assets/shaders/alpha_mask.glsl so they can be tested (and checked
* against it on the GPU).
*
* A cut-out (glTF's alphaMode MASK) is a surface with holes: a leaf, a fence's wire, a grate, drawn as one flat
* polygon whose texture's alpha says where the holes are. Each texel is either fully there or not, so no blending or
* sorting is needed: a hole is a pixel thrown away, and the rest is opaque like any surface. Such sheets are usually
* double-sided (glTF's doubleSided): seen from behind, a leaf is still a leaf.
*/

// glTF's rule: a texel whose alpha (its texture's times the material's opacity) is under the cutoff is a hole.
bool isCutAway(float alpha, float cutoff);

/*
* How a fragment's normal and tangent frame are turned: -1 for the back of a double-sided surface, which is shaded as
* the front's mirror image (its normal, tangent and bitangent all reversed, so a normal map's bumps stand out of the
* side being looked at), else 1. A one-sided surface's back is never drawn, so it keeps its frame.
*/
float facingSign(bool frontFacing, bool doubleSided);
