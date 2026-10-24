#ifndef RENDER_GPU_MATERIAL_SHARED_H
#define RENDER_GPU_MATERIAL_SHARED_H

/*
* A material's flags (GpuMaterial::extra[1], MaterialData.extra.y), shared by render/gpu/material_uniforms.cpp and the
* shaders, so the two cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// Both sides drawn (glTF's doubleSided): no back faces culled, and a back face shaded as the front's mirror image.
#define MATERIAL_DOUBLE_SIDED 1u

// A cut-out (glTF's alphaMode MASK): texels whose alpha is under the cutoff are holes.
#define MATERIAL_CUT_OUT 2u

// Blended (glTF's alphaMode BLEND): its alpha is the share of the pixel it covers (render/gpu/transparency.h).
#define MATERIAL_BLEND 4u

// Drawn by refraction (thick or frosted glass, render/gpu/transparency.h, isRefractive): it reads the blurred picture
// from before the glass where its view ray lands, instead of blending over the picture.
#define MATERIAL_REFRACTS 8u

// The specialization constant that turns a scene fragment shader into its cut-out variant, which throws away the holes.
// Number 1, after the light probes' (DDGI_SAMPLE_PROBES_CONSTANT), so a pipeline can set both.
#define MATERIAL_CUT_OUT_CONSTANT 1

#endif
