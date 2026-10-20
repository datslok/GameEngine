#include "scene/probe_volume.h"

#include <algorithm>

/*
* A cube round the point: the probes a light could reach lie inside it, along with a few in its corners that it cannot.
*/
ProbeEvent probeEventAround(ProbeEventKind kind, const Vec3& centre, float reach) {
    return probeEventAround(kind, std::span<const Vec3>{&centre, 1}, reach);
}

/*
* One box rather than an event per light: switching many lights at once (a field of lamps) restarts their region once,
* and the renderer checks each probe against one region instead of a hundred.
*/
ProbeEvent probeEventAround(ProbeEventKind kind, std::span<const Vec3> centres, float reach) {
    Vec3 minimum = centres.front();
    Vec3 maximum = centres.front();
    for (const Vec3& centre : centres) {
        minimum = Vec3{std::min(minimum.x, centre.x), std::min(minimum.y, centre.y), std::min(minimum.z, centre.z)};
        maximum = Vec3{std::max(maximum.x, centre.x), std::max(maximum.y, centre.y), std::max(maximum.z, centre.z)};
    }

    const Vec3 half{reach, reach, reach};
    return ProbeEvent{kind, false, Aabb{minimum - half, maximum + half}};
}
