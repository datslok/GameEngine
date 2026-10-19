#pragma once

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
