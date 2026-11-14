// A blended ground's layer shares, included with #include "ground_layers.glsl": the arithmetic mirrors
// render/gpu/ground_layers.cpp, which tests it, and tests/gpu checks it against it on the GPU. A pure function only: the
// layers' texture reads are in the main pass (scene_surface.glsl).

#ifndef GROUND_LAYERS_GLSL
#define GROUND_LAYERS_GLSL

#include "render/gpu/ground_layers_shared.h"

// The layers' shares from their painted weights and heights (groundBlendWeights in ground_layers.cpp): each painted
// layer's weight times how far its height comes within blendDepth of the highest painted one's, normalised.
vec4 groundBlendWeights(vec4 painted, vec4 heights, float blendDepth) {
    bvec4 isPainted = greaterThan(painted, vec4(0.0));
    float paintedTotal = dot(painted, vec4(isPainted));

    if (!(paintedTotal > 0.0)) {
        return vec4(1.0, 0.0, 0.0, 0.0);
    }

    float highest = -1.0;
    for (int layer = 0; layer < 4; ++layer) {
        if (isPainted[layer]) {
            highest = max(highest, heights[layer]);
        }
    }

    vec4 shares = vec4(isPainted) * painted * max(heights - (highest - blendDepth), vec4(0.0));
    float total = shares.x + shares.y + shares.z + shares.w;

    if (total > 0.0) {
        return shares / total;
    }

    return max(painted, vec4(0.0)) / paintedTotal;
}

// Where to read the painted weights for a cubic B-spline through the sixteen nearest texels in four straight reads
// (groundPaintTaps in ground_layers.cpp, which explains it): in and out in texels.
struct GroundPaintTaps {
    vec2 first;
    vec2 second;
    vec2 firstShare;
};

GroundPaintTaps groundPaintTaps(vec2 texelPlace) {
    vec2 centred = texelPlace - 0.5;
    vec2 cell = floor(centred);
    vec2 f = centred - cell;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
    vec2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    vec2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 firstShare = w0 + w1;
    vec2 secondShare = w2 + w3;

    GroundPaintTaps taps;
    taps.first = cell - 1.0 + w1 / firstShare + 0.5;
    taps.second = cell + 1.0 + w3 / secondShare + 0.5;
    taps.firstShare = firstShare;
    return taps;
}

// The object's own axes and how much it stretches along each, from its model matrix's 3 x 3 part (projectionFrame in
// ground_layers.cpp): the columns' lengths are the scale, the columns over them the axes; a squashed column a zero axis.
vec3 projectionScale(mat3 model) {
    return vec3(length(model[0]), length(model[1]), length(model[2]));
}

mat3 projectionAxes(mat3 model, vec3 scale) {
    return mat3(scale.x > 0.0 ? model[0] / scale.x : vec3(0.0), scale.y > 0.0 ? model[1] / scale.y : vec3(0.0),
                scale.z > 0.0 ? model[2] / scale.z : vec3(0.0));
}

// The three projections' shares from the normal in the object's frame (projectionWeights): |n|^4, normalised, the small
// ones dropped by subtracting GROUND_PROJECTION_SMALLEST and the rest scaled back to one; a zero normal reads from above.
vec3 projectionWeights(vec3 frameNormal) {
    vec3 squared = frameNormal * frameNormal;
    vec3 fourth = squared * squared;
    float total = fourth.x + fourth.y + fourth.z;

    if (!(total > 0.0)) {
        return vec3(0.0, 1.0, 0.0);
    }

    vec3 kept = max(fourth / total - float(GROUND_PROJECTION_SMALLEST), vec3(0.0));
    return kept * (1.0 / (kept.x + kept.y + kept.z));
}

// Where projection along an axis (0 x, 1 y, 2 z) reads, in metres: (z, y), (x, z) or (x, y) (projectedPlace).
vec2 projectedPlace(vec3 framePosition, int axis) {
    if (axis == 0) {
        return framePosition.zy;
    }

    if (axis == 1) {
        return framePosition.xz;
    }

    return framePosition.xy;
}

float projectionSign(float value) {
    return value < 0.0 ? -1.0 : 1.0;
}

// The bumped normal in the object's frame from each projection's decoded normal map direction, each whiteout-blended
// with the surface's normal in its own plane and turned back, blended by the weights (projectedNormal).
vec3 projectedNormal(vec3 frameNormal, vec3 tiltX, vec3 tiltY, vec3 tiltZ, vec3 weights) {
    vec3 x = vec3(tiltX.x + frameNormal.z, tiltX.y + frameNormal.y, tiltX.z * abs(frameNormal.x));
    vec3 y = vec3(tiltY.x + frameNormal.x, tiltY.y + frameNormal.z, tiltY.z * abs(frameNormal.y));
    vec3 z = vec3(tiltZ.x + frameNormal.x, tiltZ.y + frameNormal.y, tiltZ.z * abs(frameNormal.z));
    vec3 combined = vec3(x.z * projectionSign(frameNormal.x), x.y, x.x) * weights.x + vec3(y.x, y.z * projectionSign(frameNormal.y), y.y) * weights.y +
                    vec3(z.x, z.y, z.z * projectionSign(frameNormal.z)) * weights.z;
    float combinedLength = length(combined);
    return combinedLength > 0.0 ? combined / combinedLength : vec3(0.0);
}

// Where along the strata's strip a world height is (strataPlace).
float strataPlace(float height, float bottom, float inverseSpan) {
    return (height - bottom) * inverseSpan;
}

// The strata's weights with the top layer taking over facing up (strataPaint); topLayer < 0 for none.
vec4 strataPaint(vec4 strata, int topLayer, float upward, float topStart, float topFull) {
    if (topLayer < 0 || topLayer > 3) {
        return strata;
    }

    float top = topFull > topStart ? smoothstep(topStart, topFull, upward) : (upward >= topStart ? 1.0 : 0.0);
    vec4 result = strata * (1.0 - top);
    result[topLayer] += top;
    return result;
}

// Wavy beds: the height moved by the warp texel (0.5 none) up to amplitude either way (warpedStrataHeight).
float warpedStrataHeight(float height, float warpTexel, float amplitude) {
    return height + (2.0 * warpTexel - 1.0) * amplitude;
}

// Where along the streaks a point is, for a side projection (0 x, 2 z): across its plane, down the height (streakPlace).
vec2 streakPlace(vec3 framePosition, int axis, float inverseWidth, float inverseLength) {
    return vec2(projectedPlace(framePosition, axis).x * inverseWidth, framePosition.y * inverseLength);
}

// The albedo's factor under a streak (streakShade): the stain times the strength, on the sides only.
float streakShade(float streakTexel, float strength, float sideShare) {
    return 1.0 - strength * sideShare * (1.0 - streakTexel);
}

#endif
