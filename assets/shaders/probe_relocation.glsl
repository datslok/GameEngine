#ifndef PROBE_RELOCATION_GLSL
#define PROBE_RELOCATION_GLSL

// Moving the light probes off the static level and their states. Pure functions, mirrored by
// render/gpu/probe_relocation.h, which explains each; the GPU tests check them against it.

#include "render/gpu/ddgi_shared.h"

// The largest finite float, for "nothing found yet" (std::numeric_limits<float>::max() in C++).
const float PROBE_NOTHING_YET = 3.402823466e38;

bool probeTraced(int state) {
    return state == DDGI_PROBE_AWAKE || state == DDGI_PROBE_VIGILANT;
}

struct ProbeRayTally {
    int rays;
    int backFaces;
    int frontFaces;
    float closestBackFace;
    vec3 closestBackFaceDirection;
    float closestFrontFace;
    vec3 closestFrontFaceDirection;
    float farthestFrontFace;
    vec3 farthestFrontFaceDirection;
    float surfaceBeyondCells;
};

ProbeRayTally emptyProbeRayTally() {
    return ProbeRayTally(0, 0, 0, PROBE_NOTHING_YET, vec3(0.0), PROBE_NOTHING_YET, vec3(0.0), 0.0, vec3(0.0), PROBE_NOTHING_YET);
}

void tallyProbeRay(inout ProbeRayTally tally, vec3 direction, float signedDistance, vec3 start, vec3 spacing) {
    tally.rays += 1;

    if (signedDistance >= DDGI_RAY_REACH) {
        return;
    }

    if (signedDistance < 0.0) {
        tally.backFaces += 1;

        if (-signedDistance < tally.closestBackFace) {
            tally.closestBackFace = -signedDistance;
            tally.closestBackFaceDirection = direction;
        }

        return;
    }

    tally.frontFaces += 1;

    if (signedDistance < tally.closestFrontFace) {
        tally.closestFrontFace = signedDistance;
        tally.closestFrontFaceDirection = direction;
    }

    if (signedDistance > tally.farthestFrontFace) {
        tally.farthestFrontFace = signedDistance;
        tally.farthestFrontFaceDirection = direction;
    }

    vec3 beyond = abs(start + direction * signedDistance) - spacing;
    tally.surfaceBeyondCells = min(tally.surfaceBeyondCells, max(beyond.x, max(beyond.y, beyond.z)));
}

float probeEscapeScale(vec3 offset, vec3 step, vec3 limit) {
    float scale = PROBE_NOTHING_YET;

    for (int axis = 0; axis < 3; ++axis) {
        if (step[axis] > 0.0) {
            scale = min(scale, (limit[axis] - offset[axis]) / step[axis]);
        } else if (step[axis] < 0.0) {
            scale = min(scale, (-limit[axis] - offset[axis]) / step[axis]);
        }
    }

    float stepLength = length(step);
    return stepLength > 0.0 ? scale - DDGI_ESCAPE_MARGIN / stepLength : 0.0;
}

bool probeInsideSomething(ProbeRayTally tally) {
    return float(tally.backFaces) > DDGI_INSIDE_BACK_FACE_SHARE * float(tally.rays);
}

struct ProbeMove {
    vec3 offset;
    bool stuck;
};

ProbeMove relocateProbeStep(ProbeRayTally tally, vec3 offset, vec3 spacing) {
    vec3 limit = spacing * DDGI_OFFSET_LIMIT;

    if (probeInsideSomething(tally)) {
        vec3 step = tally.closestBackFaceDirection * tally.closestBackFace;
        float scale = probeEscapeScale(offset, step, limit);

        if (scale <= 1.0) {
            return ProbeMove(offset, true);
        }

        return ProbeMove(offset + step * scale, false);
    }

    float smallestSpacing = min(spacing.x, min(spacing.y, spacing.z));

    if (tally.frontFaces > 0 && tally.closestFrontFace < DDGI_MIN_FRONT_FACE_SHARE * smallestSpacing &&
        dot(tally.closestFrontFaceDirection, tally.farthestFrontFaceDirection) <= DDGI_SAME_SURFACE_COSINE) {
        vec3 moved = offset + tally.farthestFrontFaceDirection * min(tally.farthestFrontFace, DDGI_FRONT_FACE_NUDGE * smallestSpacing);
        return ProbeMove(clamp(moved, -limit, limit), false);
    }

    return ProbeMove(offset, false);
}

// Must match GpuProbeRelocation in render/gpu/probe_relocation.h.
struct ProbeRelocation {
    vec3 offset;
    float off;
    float surfaceBeyondCells;
    float unused0;
    float unused1;
    float unused2;
};

ProbeRelocation finishProbeRelocation(ProbeRayTally tally, vec3 offset, bool stuck) {
    bool off = stuck || probeInsideSomething(tally);
    return ProbeRelocation(offset, off ? 1.0 : 0.0, tally.surfaceBeyondCells, 0.0, 0.0, 0.0);
}

// Must match GpuProbeState in render/gpu/probe_relocation.h: each probe's offset from its grid point and its state, as
// every probe pass reads it.
struct ProbeState {
    vec3 offset;
    int state;
};

#endif
