#include "scene/probe_volume.h"

/*
* A cube round the point: the probes a light could reach lie inside it, along with a few in its corners that it cannot.
*/
ProbeEvent probeEventAround(ProbeEventKind kind, const Vec3& centre, float reach) {
    const Vec3 half{reach, reach, reach};
    return ProbeEvent{kind, false, Aabb{centre - half, centre + half}};
}
