// The wind force field's pure maths (render/gpu/wind.h mirrors every function here and is tested against it; no buffer
// or texture reads): value noise, the wind at a place, a spring's rest and its step, and the windows' cells.
#ifndef WIND_GLSL
#define WIND_GLSL

#include "hash.glsl"
#include "render/gpu/wind_shared.h"

// The field's record as the shaders read it. Must match GpuWindField.
struct WindField {
    vec4 wind;    // xy the direction (x, z), z the speed, w the time
    vec4 gusts;   // the gusts' strength and size, the turbulence's share and size
    vec4 step;    // x the frame's step, y 1 while the field is on
    vec4 springs[WIND_CATEGORIES]; // frequency, damping, response
    ivec4 windows[WIND_CASCADES];  // this frame's first cell (x, z), last frame's
    vec4 cells[WIND_CASCADES];     // a cell (m), 1 / the window's width (m)
    ivec4 flags;  // x 1 when last frame's windows are the ones the springs hold
};

// A lattice point's value: its hash's top 24 bits over 2^24 (windNoise's).
float windLatticeValue(ivec3 point) {
    return float(pcg3d(uvec3(point)).x >> 8u) * (1.0 / 16777216.0);
}

// Value noise in 3D, smoothstep-blended between the eight lattice points round a place (windNoise).
float windNoise(vec3 place) {
    vec3 lower = floor(place);
    ivec3 corner = ivec3(lower);
    vec3 fraction = place - lower;
    vec3 share = fraction * fraction * (3.0 - 2.0 * fraction);
    float near = mix(mix(windLatticeValue(corner), windLatticeValue(corner + ivec3(1, 0, 0)), share.x),
                     mix(windLatticeValue(corner + ivec3(0, 1, 0)), windLatticeValue(corner + ivec3(1, 1, 0)), share.x), share.y);
    float far = mix(mix(windLatticeValue(corner + ivec3(0, 0, 1)), windLatticeValue(corner + ivec3(1, 0, 1)), share.x),
                    mix(windLatticeValue(corner + ivec3(0, 1, 1)), windLatticeValue(corner + ivec3(1, 1, 1)), share.x), share.y);
    return mix(near, far, share.z);
}

/*
* The wind at a place on the ground (x and z) and a time (windVelocity): along is its unit direction, gusts the gusts'
* strength and size and the turbulence's share and size.
*/
vec2 windVelocity(vec2 along, float speed, vec4 gusts, vec2 place, float seconds) {
    vec2 side = vec2(-along.y, along.x);
    float gustSize = max(gusts.y, 1e-3);
    float turbulenceSize = max(gusts.w, 1e-3);
    vec2 upwind = (place - along * (speed * seconds)) * (1.0 / gustSize);
    float gust = windNoise(vec3(upwind, float(WIND_GUST_EVOLUTION) * speed * seconds / gustSize));
    float windSpeed = speed * max(0.0, 1.0 + gusts.x * (2.0 * gust - 1.0));
    vec2 eddy = place * (1.0 / turbulenceSize);
    float swirl = windNoise(vec3(eddy.x + float(WIND_TURBULENCE_OFFSET), eddy.y, speed * seconds / turbulenceSize));
    return along * windSpeed + side * (gusts.z * speed * (2.0 * swirl - 1.0));
}

// Where a spring of a response rests in a wind (windSpringTarget).
vec2 windSpringTarget(vec2 velocity, float response) {
    return velocity * (length(velocity) * response);
}

// A spring's state (xy displacement, zw velocity) after seconds pulled towards target (windSpringStep).
vec4 windSpringStep(vec4 state, vec2 target, float frequency, float damping, float seconds) {
    float step = clamp(seconds, 0.0, float(WIND_LONGEST_STEP));
    float omega = 6.28318530718 * frequency;
    int substeps = clamp(int(ceil(omega * step / float(WIND_SUBSTEP_TURN))), 1, WIND_MOST_SUBSTEPS);
    float h = step / float(substeps);
    vec2 displacement = state.xy;
    vec2 velocity = state.zw;

    for (int substep = 0; substep < substeps; ++substep) {
        velocity = velocity + ((target - displacement) * (omega * omega) - velocity * (2.0 * damping * omega)) * h;
        displacement = displacement + velocity * h;
    }

    return vec4(displacement, velocity);
}

// The floor of a over b for whole numbers, negatives included.
int windFloorDivide(int a, int b) {
    return int(floor(float(a) / float(b)));
}

// The cell of a window (its first cell origin) a texel holds (windCellOfTexel).
ivec2 windCellOfTexel(ivec2 texel, ivec2 origin) {
    ivec2 offset = texel - origin;
    return origin + offset - WIND_CELLS * ivec2(windFloorDivide(offset.x, WIND_CELLS), windFloorDivide(offset.y, WIND_CELLS));
}

bool windCellInWindow(ivec2 cell, ivec2 origin) {
    return all(greaterThanEqual(cell, origin)) && all(lessThan(cell, origin + WIND_CELLS));
}

#endif
