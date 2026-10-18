#pragma once

#include "math/vec3.h"
#include "math/vec4.h"

/*
* The overlay (the debug UI, later the HUD) is drawn into its own 8-bit image, then laid over the finished screen image
* by overlay.frag. ImGui writes sRGB colours and blends them the way a plain 8-bit screen would; onto an image cleared
* to transparent its blending leaves each texel premultiplied (colour times coverage, and the coverage). So each texel
* is turned back into a colour, decoded to linear light, scaled to the screen's white level (1 on an SDR screen, the
* desktop's white on an HDR one, where scRGB's 1 is a dim 80 nits) and premultiplied again for the blend
* (src + dst * (1 - src alpha)).
*
* These functions mirror overlay.frag and the overlay pipeline's blend state, so tests can check the maths.
*/

// What overlay.frag writes for one overlay texel: premultiplied linear colour in screen units, and the coverage.
Vec4 overlayOutput(const Vec4& texel, float whiteLevel);

// The blend: the overlay's output over a screen pixel's colour.
Vec3 blendOverlay(const Vec3& screen, const Vec4& output);
