#ifndef RENDER_GPU_EYE_ADAPTATION_SHARED_H
#define RENDER_GPU_EYE_ADAPTATION_SHARED_H

// Eye adaptation's numbers, shared by render/gpu/eye_adaptation.h and the shaders (eye_adaptation.glsl and its passes).
// Only #defines of plain numbers, since GLSL reads them too.

// The histogram of the scene's brightness: this many bins, evenly spaced in stops (log2 of luminance) from the lowest
// to the highest, a tenth of a stop each. Darker pixels fall into the first bin, brighter into the last.
#define EYE_HISTOGRAM_BINS 256
#define EYE_HISTOGRAM_LOWEST_EV -16.0
#define EYE_HISTOGRAM_HIGHEST_EV 9.6

// The share of pixels, darkest first, below which the mean ignores them (the black sky, deep shadow) and above which
// it ignores them too (the sun and moon, lamp bulbs): the mean is taken over the pixels between.
#define EYE_LOW_SHARE 0.5
#define EYE_HIGH_SHARE 0.9

// The histogram pass's workgroups: a square of this many pixels a side, one pixel an invocation.
#define EYE_HISTOGRAM_GROUP_SIDE 16

#endif
