// Impostors' pure maths (scene/impostor.h mirrors every function and is tested against it): the hemi-octahedral fold,
// each frame's direction and plane, where a card point falls in a frame and in the atlas, the surface behind it and the
// sun's test against a frame's depths (the plant's own shadow). No texture reads here;
// impostor_sampling.glsl reads the atlases with these.
#ifndef IMPOSTOR_GLSL
#define IMPOSTOR_GLSL

// The upper hemisphere folded onto the unit square (straight up the middle, the horizon its edge).
vec2 hemiOctahedralEncode(vec3 direction) {
    vec3 above = vec3(direction.x, max(direction.y, 0.0), direction.z);
    float sum = abs(above.x) + above.y + abs(above.z);
    float x = sum > 0.0 ? above.x / sum : 0.0;
    float z = sum > 0.0 ? above.z / sum : 0.0;
    return vec2(x + z, x - z) * 0.5 + 0.5;
}

vec3 hemiOctahedralDecode(vec2 square) {
    float u = square.x * 2.0 - 1.0;
    float v = square.y * 2.0 - 1.0;
    float x = (u + v) * 0.5;
    float z = (u - v) * 0.5;
    return normalize(vec3(x, max(1.0 - abs(x) - abs(z), 0.0), z));
}

// A frame's direction from its grid place (column, row), frames a side, corner to corner.
vec3 impostorFrameDirection(vec2 frame, float frames) {
    return hemiOctahedralDecode(frame / (frames - 1.0));
}

// A frame's plane's right (level, along x from straight above), as impostorFrameBasis.
vec3 impostorFrameRight(vec3 direction) {
    vec3 across = cross(vec3(0.0, 1.0, 0.0), direction);
    return dot(across, across) > 1e-8 ? normalize(across) : vec3(1.0, 0.0, 0.0);
}

// And its up, square to it and the direction.
vec3 impostorFrameUp(vec3 direction, vec3 right) {
    return normalize(cross(direction, right));
}

// Where a card point (offset from the middle over the radius, in the tree's frame) falls in a frame, 0..1.
vec2 impostorFrameUv(vec3 cardPoint, vec3 frameDirection) {
    vec3 right = impostorFrameRight(frameDirection);
    vec3 up = impostorFrameUp(frameDirection, right);
    return vec2(dot(cardPoint, right), dot(cardPoint, up)) * 0.5 + 0.5;
}

// A frame's texture coordinates in the atlas.
vec2 impostorAtlasUv(vec2 frameUv, vec2 frame, float frames) {
    return (frame + frameUv) / frames;
}

// A frame's depth (over the radius, along its direction from the middle) from the normal atlas's alpha, 0..1.
float impostorDepthDecode(float stored) {
    return stored * 2.0 - 1.0;
}

// The surface a frame sees behind a card point: the point moved along the frame's direction to the depth there.
vec3 impostorSurfacePoint(vec3 cardPoint, vec3 frameDirection, float depth) {
    return cardPoint + frameDirection * (depth - dot(cardPoint, frameDirection));
}

// The sun's share at a point pointDepth along a sun frame, from a 2 x 2 of texels in textureGather's order ((0, 1), (1, 1),
// (1, 0), (0, 0)): each lit where empty (covered below 0.5) or no nearer the sun than the point plus the bias; bilinear.
float impostorGatherLit(vec4 storedDepths, vec4 covered, vec2 fraction, float pointDepth, float bias) {
    vec4 weights = vec4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0 - fraction.y),
                        (1.0 - fraction.x) * (1.0 - fraction.y));
    vec4 depths = storedDepths * 2.0 - 1.0;
    vec4 lit = vec4(lessThan(covered, vec4(0.5))) + vec4(lessThanEqual(depths, vec4(pointDepth + bias)));
    return dot(weights, min(lit, vec4(1.0)));
}

// The bias in depth of biasTexels of a frame frameTexels a side (two radii across).
float impostorShadowBias(float biasTexels, float frameTexels) {
    return biasTexels * 2.0 / frameTexels;
}

// The sun's test's point (normal offset): moved off its leaf to the side facing the sun by the bias times the sine of
// the angle between them, so a leaf the sun grazes is not shadowed by its own texels.
vec3 impostorShadowPoint(vec3 point, vec3 normal, vec3 sunDirection, float bias) {
    float facing = dot(normal, sunDirection);
    float sine = sqrt(max(1.0 - facing * facing, 0.0));
    return point + normal * ((facing < 0.0 ? -bias : bias) * sine);
}

#endif
