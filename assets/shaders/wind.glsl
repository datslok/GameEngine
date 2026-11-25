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
    vec4 springs[WIND_SPRINGS]; // frequency, damping, response
    ivec4 windows[WIND_CASCADES];  // this frame's first cell (x, z), last frame's
    vec4 cells[WIND_CASCADES];     // a cell (m), 1 / the window's width (m)
    ivec4 flags;  // x 1 when last frame's windows are the ones the springs hold
    vec4 sway;    // x 1 while things sway, y and z the fade's start and end (m), w last frame's time
    vec4 flutters[WIND_CATEGORIES]; // each category's flutter: hertz, metres
    vec4 bones[2]; // the bones' motion: stiffness exponent, most stiffness, wood speed, slowest hertz; fastest hertz, swing along, swing across
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

// How much of the sway is left at a distance from the camera (windSwayFade).
float windSwayFade(float distance, float start, float end) {
    return 1.0 - smoothstep(start, end, distance);
}

// A lean softly held under WIND_MOST_LEAN (windSoftLean).
vec2 windSoftLean(vec2 lean) {
    return lean * (1.0 / (1.0 + length(lean) / float(WIND_MOST_LEAN)));
}

// The lean a spring gives something bending by bend per radian of it, softly held and faded (windFadedLean).
vec2 windFadedLean(vec2 spring, float bend, float fade) {
    return windSoftLean(spring * bend) * fade;
}

// How much a place reads a window (its first cell origin) rather than the next coarser one (windWindowShare).
float windWindowShare(vec2 place, ivec2 origin, float cellMetres) {
    vec2 cell = place / cellMetres;
    vec2 fromEdge = min(cell - vec2(origin), vec2(origin + WIND_CELLS) - cell);
    return clamp((min(fromEdge.x, fromEdge.y) - 1.0) / float(WIND_BLEND_CELLS), 0.0, 1.0);
}

// A smooth phase changing once every metres along the world (windPhase).
float windPhase(vec3 place, float metres) {
    return (place.x * 0.71 + place.y * 0.37 + place.z * 0.59) * (6.28318530718 / metres);
}

// A leaf's shiver within amount, at rate's hertz (x), its phase turning every rate's metres (y) (windFlutterOffset).
vec3 windFlutterOffset(vec3 place, float amount, vec2 rate, float seconds) {
    float turn = 6.28318530718 * rate.x * seconds;
    float phase = windPhase(place, rate.y);
    return vec3(sin(turn + phase), sin(1.3 * turn + 0.7 * phase + 2.0), sin(0.9 * turn + 1.3 * phase + 4.0)) * amount;
}

// How far a vertex's leaves flutter: flutter times the lean, growing with its height share to the top (windFlutterAmount).
float windFlutterAmount(float flutter, vec2 lean, float modelHeight, float height) {
    return flutter * length(lean) * min(max(modelHeight, 0.0) / max(height, 1e-4), 1.0);
}

// The farthest a leaf's flutter moves it: sqrt(3) |flutter| |lean| (windFlutterReach).
float windFlutterReach(float flutter, vec2 lean) {
    return 1.7320508 * abs(flutter) * length(lean);
}

/*
* Where the wind moves a vertex (windSwayOffset): fromFoot the vertex from its foot in the world, modelHeight its
* height in the mesh's frame, place where it is, sway the instance's (category, height, bend, flutter), scale the
* model's, spring its spring's lean, fade windSwayFade's, flutterRate its category's (hertz, metres), seconds the
* field's time.
*/
vec3 windSwayOffset(vec3 fromFoot, float modelHeight, vec3 place, vec4 sway, float scale, vec2 spring, float fade, vec2 flutterRate, float seconds) {
    if (sway.x < 0.0) {
        return vec3(0.0);
    }

    float share = max(modelHeight, 0.0) / max(sway.y, 1e-4);
    float heightMetres = sway.y * scale;
    vec2 lean = windFadedLean(spring, sway.z, fade);
    vec2 sideways = lean * (heightMetres * share * share);
    vec3 bent = vec3(fromFoot.x + sideways.x, fromFoot.y, fromFoot.z + sideways.y);
    float bentLength = length(bent);
    vec3 kept = bentLength > 0.0 ? bent * (length(fromFoot) / bentLength) : bent;
    vec3 offset = kept - fromFoot;

    float amount = length(lean);
    float grow = min(share, 1.0);
    float trunkDistance = sqrt(fromFoot.x * fromFoot.x + fromFoot.z * fromFoot.z);
    float branchTurn = 6.28318530718 * float(WIND_BRANCH_HERTZ) * seconds + windPhase(place, float(WIND_BRANCH_METRES));
    offset.y += float(WIND_BRANCH_SHARE) * amount * trunkDistance * grow * sin(branchTurn);

    return offset + windFlutterOffset(place, windFlutterAmount(sway.w, lean, modelHeight, sway.y), flutterRate, seconds);
}

// The farthest windSwayOffset moves any vertex of a part of a swaying thing: top its highest height in the mesh's frame,
// trunkDistance its farthest from the trunk in the world, across (windSwayReach).
float windSwayReach(vec4 sway, float scale, float top, float trunkDistance) {
    if (sway.x < 0.0) {
        return 0.0;
    }

    float share = max(top, 0.0) / max(sway.y, 1e-4);
    float grow = min(share, 1.0);
    float bend = 2.0 * sway.y * scale * share * share;
    float bob = float(WIND_BRANCH_SHARE) * max(trunkDistance, 0.0) * grow;
    float flutter = 1.7320508 * abs(sway.w) * grow;
    return float(WIND_MOST_LEAN) * (bend + bob + flutter);
}

// A hanging vertex's own swing and flutter (windSwingOffset).
vec3 windSwingOffset(float arm, vec3 place, float swing, float flutter, vec2 spring, float fade, vec2 flutterRate, float seconds) {
    vec2 lean = windFadedLean(spring, swing, fade);
    float reach = max(arm, 0.0);
    vec2 outwards = lean * reach;
    float rise = reach - sqrt(max(reach * reach - dot(outwards, outwards), 0.0));
    return vec3(outwards.x, rise, outwards.y) + windFlutterOffset(place, flutter * length(lean), flutterRate, seconds);
}

// The farthest windSwingOffset moves a hanging vertex of arm at most arm, at any spring, fade and time (windSwingReach).
float windSwingReach(float arm, float flutter) {
    float lean = min(float(WIND_MOST_LEAN), 1.0);
    float swung = max(arm, 0.0) * sqrt(max(2.0 - 2.0 * sqrt(1.0 - lean * lean), 0.0));
    return swung + windFlutterReach(flutter, vec2(lean, 0.0));
}

// A hanging chain of a length's slowest swing, Hz (windChainHertz).
float windChainHertz(float swingLength) {
    return sqrt(float(WIND_GRAVITY) / max(swingLength, float(WIND_SHORTEST_SWING))) / float(WIND_CHAIN_PERIOD);
}

// A strand's share of each hanging band, bands their frequencies falling (windChainShares).
vec3 windChainShares(float swingLength, vec3 bands) {
    float hertz = windChainHertz(swingLength);

    if (hertz >= bands.x) {
        return vec3(1.0, 0.0, 0.0);
    }

    if (hertz <= bands.z) {
        return vec3(0.0, 0.0, 1.0);
    }

    if (hertz >= bands.y) {
        float share = log(bands.x / hertz) / log(bands.x / bands.y);
        return vec3(1.0 - share, share, 0.0);
    }

    float share = log(bands.y / hertz) / log(bands.y / bands.z);
    return vec3(0.0, 1.0 - share, share);
}

// A hanging vertex's whole motion (windHangingOffset).
vec3 windHangingOffset(vec3 first, vec3 second, float along, vec3 swing) {
    return first + (second - first) * along + swing;
}

#endif
