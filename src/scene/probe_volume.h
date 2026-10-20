#pragma once

#include "math/aabb.h"
#include "math/vec3.h"

// How many probes a volume has along each axis.
struct ProbeCounts {
    int x = 1;
    int y = 1;
    int z = 1;
};

/*
* A box of light probes for global illumination (render/gpu/ddgi.h): a regular grid of points where the renderer
* measures the light arriving from every direction, so surfaces between them can look up the light that bounced
* around the level. A component: a game places one where its light matters, as it places lights. Probes sit on the
* grid's corners, the first at origin and the rest spacing apart along each axis, so the volume spans
* (counts - 1) * spacing. Each room-like space needs at least one full cell of 8 probes inside it; 1-2 m apart suits
* human-scale levels.
*/
struct ProbeVolume {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 spacing{1.0f, 1.0f, 1.0f};
    ProbeCounts counts;
};

// What changed: light only (a lamp switched, the time of day jumped), or the level's shape (a wall fell), which moves
// what the probes see as well as how bright it is.
enum class ProbeEventKind {
    LightChange,
    GeometryChange
};

/*
* A scene event (render/gpu/ddgi.h, advanceProbeRestarts): a big change the game knows about, which the probes would
* otherwise only follow over their memory of seconds, or (the adaptive memory) once they can tell it from their noise.
* The probes in its region restart their average this frame, so they take the new light at once. A component that lasts
* one frame (signalProbeEvent, scene/lighting.h); keep the region to where the change matters, since a restarted probe
* is noisier for a moment.
*/
struct ProbeEvent {
    ProbeEventKind kind = ProbeEventKind::LightChange;
    bool everywhere = true; // every probe; otherwise those inside region
    Aabb region;
};

// An event for the probes within reach of a point along each axis, such as a light's range round it.
ProbeEvent probeEventAround(ProbeEventKind kind, const Vec3& centre, float reach);
