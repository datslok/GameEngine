// The cascade whose probes include probe number probe (render/gpu/probe_cascades.h, probeCascadeHolding): the last of
// the cascades whose first probe is probe or less, walked since the cascades differ in size (at most DDGI_MAX_CASCADES).
// It reads probeCascades[], so a pass includes this after declaring that buffer.
#ifndef PROBE_CASCADE_HOLDING_GLSL
#define PROBE_CASCADE_HOLDING_GLSL

int probeCascadeHolding(int probe, int cascades) {
    int holding = 0;

    for (int cascade = 1; cascade < cascades; ++cascade) {
        if (probeCascades[cascade].scroll.w <= probe) {
            holding = cascade;
        }
    }

    return holding;
}

#endif
