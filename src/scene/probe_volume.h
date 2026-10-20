#pragma once

#include "math/aabb.h"
#include "math/vec3.h"

#include <span>

// How many probes a volume has along each axis.
struct ProbeCounts {
    int x = 1;
    int y = 1;
    int z = 1;
};

/*
* How far a volume's storage is turned along each axis, as a circular buffer: the probe at grid place l (counted from
* the first probe) is kept in slot (l + scroll) mod count. A window that follows the camera moves by changing this
* instead of copying its probes, so only the plane it brings in starts afresh. 0 for a volume that never moves.
*/
struct ProbeScroll {
    int x = 0;
    int y = 0;
    int z = 0;
};

/*
* A box of light probes for global illumination (render/gpu/ddgi.h): a regular grid of points where the renderer
* measures the light arriving from every direction, so surfaces between them can look up the light that bounced
* around the level. Probes sit on the grid's corners, the first at origin and the rest spacing apart along each axis,
* so the volume spans (counts - 1) * spacing. The renderer's cascades (render/gpu/probe_cascades.h) are such volumes,
* following the camera; games place ProbeCascades, not volumes.
*/
struct ProbeVolume {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 spacing{1.0f, 1.0f, 1.0f};
    ProbeCounts counts;
    ProbeScroll scroll;

    // Most volumes never scroll, so the scroll may be left out.
    ProbeVolume() = default;
    ProbeVolume(const Vec3& origin, const Vec3& spacing, const ProbeCounts& counts, const ProbeScroll& scroll = {})
        : origin(origin), spacing(spacing), counts(counts), scroll(scroll) {}
};

/*
* Global illumination for a world: a component a game places once, as it places lights. The renderer keeps light
* probes in nested grids (cascades) round a point, the camera by default: the densest near it and each further one
* twice as coarse and twice as far, all moving with it, so light bounces everywhere the camera goes with no probes to
* place per level. How dense and how many is the quality settings' choice (GlobalIlluminationSettings), not the game's.
* A game that follows something other than the camera (a MOBA's ground under the view) sets centre each frame.
*/
struct ProbeCascades {
    bool followCamera = true;
    Vec3 centre{0.0f, 0.0f, 0.0f}; // followed when not the camera
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

// One event for lights switched together (at least one point): the box round every point's reach.
ProbeEvent probeEventAround(ProbeEventKind kind, std::span<const Vec3> centres, float reach);
