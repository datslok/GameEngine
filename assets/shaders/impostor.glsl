// Impostors' pure maths (scene/impostor.h mirrors every function and is tested against it): the hemi-octahedral fold,
// each frame's direction and plane, where a card point falls in a frame and in the atlas. No texture reads here;
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

#endif
