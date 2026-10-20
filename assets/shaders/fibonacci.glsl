#ifndef FIBONACCI_GLSL
#define FIBONACCI_GLSL

// Points spread evenly over the sphere (a spherical Fibonacci spiral), shared by the light probes' rays and the sky's
// averages. Mirrored by sphericalFibonacci in render/gpu/ddgi.h, which explains it.

vec3 sphericalFibonacci(int index, int count) {
    const float goldenFraction = 0.6180339887;
    const float fullTurn = 6.28318530718;
    float z = 1.0 - (2.0 * float(index) + 1.0) / float(count);
    float angle = fullTurn * fract(float(index) * goldenFraction);
    float radius = sqrt(max(0.0, 1.0 - z * z));
    return vec3(radius * cos(angle), radius * sin(angle), z);
}

#endif
