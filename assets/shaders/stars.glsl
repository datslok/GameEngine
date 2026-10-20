#ifndef STARS_GLSL
#define STARS_GLSL

// Drawing the stars. Pure functions, mirrored by render/gpu/stars.h, which explains each; the GPU tests check them
// against it.

float starLight(float magnitude, float brightness, float contrast) {
    return brightness * pow(10.0, -0.4 * contrast * magnitude);
}

vec2 starScreenPosition(vec3 right, vec3 up, vec3 forward, vec3 direction) {
    float along = dot(direction, forward);
    return vec2(dot(direction, right) / (dot(right, right) * along), dot(direction, up) / (dot(up, up) * along));
}

float starSplatWeight(vec2 offset) {
    return max(1.0 - abs(offset.x), 0.0) * max(1.0 - abs(offset.y), 0.0);
}

float pixelSolidAngle(vec3 right, vec3 up, float width, float height, float cosOffAxis) {
    return (2.0 * length(right) / width) * (2.0 * length(up) / height) * cosOffAxis * cosOffAxis * cosOffAxis;
}

#endif
