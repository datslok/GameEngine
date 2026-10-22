#ifndef RENDER_GPU_TONE_MAPPING_SHARED_H
#define RENDER_GPU_TONE_MAPPING_SHARED_H

/*
* The tone mapping curves' numbers, shared by render/gpu/tone_mapping.cpp (the tested mirror) and tone_map.frag, so the
* two cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// Which curve: the values of ToneMapCurve, sent to the shader as a number.
#define TONE_MAP_CLAMP 0
#define TONE_MAP_REINHARD 1
#define TONE_MAP_ACES 2
#define TONE_MAP_AGX 3

// Krzysztof Narkowicz's fit of the ACES film curve: (x (a x + b)) / (x (c x + d) + e).
#define ACES_A 2.51
#define ACES_B 0.03
#define ACES_C 2.43
#define ACES_D 0.59
#define ACES_E 0.14

// AgX (Troy Sobotka), in the minimal form by Benjamin Wrensch. Colours are first pulled slightly towards grey (the inset
// matrix, given by rows), so very bright saturated lights roll off towards white instead of clipping to a pure primary.
#define AGX_INSET_00 0.842479062253094
#define AGX_INSET_01 0.0784335999999992
#define AGX_INSET_02 0.0792237451477643
#define AGX_INSET_10 0.0423282422610123
#define AGX_INSET_11 0.878468636469772
#define AGX_INSET_12 0.0791661274605434
#define AGX_INSET_20 0.0423756549057051
#define AGX_INSET_21 0.0784336
#define AGX_INSET_22 0.879142973793104

// The inset's inverse (by rows), which brings the colours back out after the curve.
#define AGX_OUTSET_00 1.19687900512017
#define AGX_OUTSET_01 -0.0980208811401368
#define AGX_OUTSET_02 -0.0990297440797205
#define AGX_OUTSET_10 -0.0528968517574562
#define AGX_OUTSET_11 1.15190312990417
#define AGX_OUTSET_12 -0.0989611768448433
#define AGX_OUTSET_20 -0.0529716355144438
#define AGX_OUTSET_21 -0.0980434501171241
#define AGX_OUTSET_22 1.15107367264116

// The range of brightness the curve covers, in stops (powers of two) around 1: about 12.5 below to 4 above.
#define AGX_MIN_STOPS -12.47393
#define AGX_MAX_STOPS 4.026069

// The S-shaped curve over that range, a sixth-degree polynomial: c6 t^6 + c5 t^5 + ... + c0.
#define AGX_C6 15.5
#define AGX_C5 -40.14
#define AGX_C4 31.96
#define AGX_C3 -6.868
#define AGX_C2 0.4298
#define AGX_C1 0.1191
#define AGX_C0 -0.00232

// The curve's output is display-encoded with a plain 2.2 gamma; raising it to this power gives linear light back.
#define AGX_DISPLAY_GAMMA 2.2

// Night vision. Linear sRGB to CIE XYZ's X and Z (D65; Y is the luminance, pixelLuminance), by rows.
#define SRGB_TO_X_RED 0.4124
#define SRGB_TO_X_GREEN 0.3576
#define SRGB_TO_X_BLUE 0.1805
#define SRGB_TO_Z_RED 0.0193
#define SRGB_TO_Z_GREEN 0.1192
#define SRGB_TO_Z_BLUE 0.9505

// Ward Larson, Rushmeier and Piatko's (1997) fit of the rods' sensitivity: Y (A (1 + (Y + Z) / X) - B).
#define SCOTOPIC_A 1.33
#define SCOTOPIC_B 1.68

// The colour the rods' grey takes at full blue shift (by eye; scaled to a luminance of 1 where it is used): moonlit
// scenes look cool, though the rods see no colour at all.
#define NIGHT_TINT_RED 0.75
#define NIGHT_TINT_GREEN 0.9
#define NIGHT_TINT_BLUE 1.35

#endif
