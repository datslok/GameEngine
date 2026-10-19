#pragma once

/*
* The depth prepass draws every opaque object writing depth only, so the depth image holds the nearest surface at every
* pixel before anything is shaded. The main pass then shades only the fragment whose depth equals it: each pixel runs
* the costly lighting once, whatever order the objects are drawn in. EQUAL works only because both passes compute
* every vertex's position with the same shader (triangle.vert, gl_Position declared invariant), bit for bit.
*/

// The depth tests the passes use. Reversed-Z (gpu_depth_range.h): nearer surfaces have greater depths.
enum class DepthCompare {
    Greater,
    Equal,
};

struct DepthState {
    DepthCompare compare = DepthCompare::Greater;
    bool write = true;
};

// The prepass keeps the nearest depth, as the main pass does on its own.
DepthState prepassDepthState();

// After a prepass the depth image is final: only the fragment at exactly that depth is shaded, and nothing is written.
DepthState mainPassDepthState(bool afterPrepass);
