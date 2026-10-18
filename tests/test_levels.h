#pragma once

#include "math/transform.h"
#include "physics/static_level.h"
#include "scene/collision_mesh.h"
#include "scene/mesh.h"

#include <cmath>
#include <numbers>

/*
* Small levels and comparisons shared by the physics tests, so each test file does not grow its own copy.
*/

inline bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

// A solid box from minimum to maximum, as level triangles (the unit cube stretched and moved).
inline TriangleMesh block(const Vec3& minimum, const Vec3& maximum) {
    const Vec3 centre = (minimum + maximum) * 0.5f;
    const Vec3 half = (maximum - minimum) * 0.5f;
    return collisionMesh(Mesh::cube(), Mat4::translation(centre.x, centre.y, centre.z) * Mat4::scaling(half.x, half.y, half.z));
}

// A level with a big floor whose top is at y = 0.
inline StaticLevel flatLevel() {
    StaticLevel level;
    level.add(block(Vec3{-50.0f, -1.0f, -50.0f}, Vec3{50.0f, 0.0f, 50.0f}));
    return level;
}

// A ramp rising towards +z from z = 0 at the given angle, its top surface starting at height 0, `length` long along
// the slope, 4 wide around x = 0, ending in a platform at the ramp's top height. Returns that height.
inline float addRamp(StaticLevel& level, float degrees, float length) {
    const float slope = degrees * std::numbers::pi_v<float> / 180.0f;
    const float rise = length * std::sin(slope);
    const float run = length * std::cos(slope);
    constexpr float thickness = 0.4f;
    // The slab is tipped about x by -slope, so the +z end goes up; its top face passes through the origin.
    const Vec3 topCentre{0.0f, rise * 0.5f, run * 0.5f};
    const Vec3 upAlongNormal = Vec3{0.0f, std::cos(slope), -std::sin(slope)} * (thickness * 0.5f);
    const Vec3 centre = topCentre - upAlongNormal;
    level.add(collisionMesh(Mesh::cube(), Mat4::translation(centre.x, centre.y, centre.z) * Mat4::rotationX(-slope)
                                              * Mat4::scaling(2.0f, thickness * 0.5f, length * 0.5f)));
    level.add(block(Vec3{-2.0f, -1.0f, run}, Vec3{2.0f, rise, run + 6.0f}));
    return rise;
}
