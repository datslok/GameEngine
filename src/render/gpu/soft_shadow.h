#pragma once

#include "render/gpu/shader_shared.h"

/*
* Soft shadows sized by the light (percentage-closer soft shadows, PCSS). A real light glows over an area, not a point,
* so behind an object's edge there is a band where part of the glowing area is hidden (the penumbra), and it widens the
* further the receiving surface is behind the blocker, like the moon's shadow on Earth during an eclipse.
*
* The shader does it in three steps, for each point and light: search the shadow map around the point for blockers and
* average how far they are from the light; from that, work out how wide the penumbra is here; then filter (average many
* depth comparisons) over that width. These functions are the shader's arithmetic, kept here so it can be tested;
* triangle.frag mirrors them. All widths are in texels of the light's tile.
*/

// The narrowest filter (about the old fixed 3x3 softness) and the widest. Wider filters cost no more samples but look
// grainier, and they must stay well inside a tile's edge margin of samples. Shared with the shader.
inline constexpr float minShadowFilterTexels = static_cast<float>(MIN_SHADOW_FILTER_TEXELS);
inline constexpr float maxShadowFilterTexels = static_cast<float>(MAX_SHADOW_FILTER_TEXELS);
inline constexpr float maxBlockerSearchTexels = static_cast<float>(MAX_BLOCKER_SEARCH_TEXELS);

// Depth from the shadow map (the GPU's reversed 1..0) back to distance from the light along the view's axis.
// Reversed perspective depth is n (f - d) / (d (f - n)), so d = f n / (n + depth (f - n)).
float linearShadowDepth(float depth, float nearPlane, float farPlane);

/*
* How far around the point to look for blockers. A blocker at distance z from the light can hide part of an emitter of
* radius R from a point at distance d if it lies within R (1/z - 1/d) of the point's direction (in units of the view's
* width per distance). That is widest for blockers right at the near plane. texelPerUnit is the world size of one texel
* per unit of distance from the light.
*/
float blockerSearchTexels(float emitterRadius, float receiverDepth, float nearPlane, float texelPerUnit);

// The penumbra's width at the receiver, R (d - z) / z, measured in the texels that cover d * texelPerUnit each there.
float penumbraTexels(float emitterRadius, float receiverDepth, float blockerDepth, float texelPerUnit);
