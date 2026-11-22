// Unit quaternions (x y z w, as glTF), included with #include "quat.glsl": mirrors math/quat.h.

#ifndef QUAT_GLSL
#define QUAT_GLSL

// a * b: turns by b first, then a (Quat::operator*).
vec4 quatMultiply(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

// A vector turned by a unit quaternion (Quat::rotate): v + 2w (u x v) + u x (2 u x v).
vec3 quatRotate(vec4 q, vec3 v) {
    vec3 twiceCross = 2.0 * cross(q.xyz, v);
    return v + q.w * twiceCross + cross(q.xyz, twiceCross);
}

// The turn by |rotation| radians about rotation's direction (Quat::fromRotationVector); none for a zero vector.
vec4 quatFromRotationVector(vec3 rotation) {
    float angle = length(rotation);

    if (angle == 0.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    return vec4(rotation * (sin(angle * 0.5) / angle), cos(angle * 0.5));
}

#endif
