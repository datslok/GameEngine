// A blended ground's layer shares, included with #include "ground_layers.glsl": the arithmetic mirrors
// render/gpu/ground_layers.cpp, which tests it, and tests/gpu checks it against it on the GPU. A pure function only: the
// layers' texture reads are in the main pass (scene_surface.glsl).

#ifndef GROUND_LAYERS_GLSL
#define GROUND_LAYERS_GLSL

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

#endif
