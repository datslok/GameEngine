// How much of a light reaches a point, included with #include "lighting.glsl": distance falloff, the spotlight cone and
// the cube face a point light's shadow is read from. Mirrors render/gpu/light_falloff.cpp and pointShadowFace in
// render/gpu/shadow_map.cpp, which test the maths; tests/gpu checks these functions against them on the GPU.

#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

/*
* Inverse square falloff, softened near the light by the size of its source (a point light uses 1, which keeps it finite at the light),
* times a window that reaches exactly 0 at range with no visible edge.
*/
float distanceFalloff(float distance, float range, float sourceRadius) {
    float ratio = distance / range;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance * distance + sourceRadius * sourceRadius);
}

/*
* How close a point is to a spotlight's axis, from the cosine of the angle between them: full inside the inner cone,
* fading smoothly to nothing at the outer cone, like a real flashlight's soft edge.
*/
float spotCone(float alongBeam, float cosOuter, float cosInner) {
    return smoothstep(cosOuter, cosInner, alongBeam);
}

/*
* The face whose camera sees this direction from the light: the axis of the largest component, in the order
* +X, -X, +Y, -Y, +Z, -Z. Must match pointShadowFace in render/gpu/shadow_map.cpp.
*/
int pointShadowFace(vec3 fromLight) {
    vec3 size = abs(fromLight);

    if (size.x >= size.y && size.x >= size.z) {
        return fromLight.x > 0.0 ? 0 : 1;
    }

    if (size.y >= size.z) {
        return fromLight.y > 0.0 ? 2 : 3;
    }

    return fromLight.z > 0.0 ? 4 : 5;
}

#endif
