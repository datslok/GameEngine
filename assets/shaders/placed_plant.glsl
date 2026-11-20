// A plant the planting cells placed (render/gpu/planting_records.h, GpuPlacedPlant), as their passes write it and the
// placed drawing reads it (placed_drawing.glsl). The struct alone, so any shader can include it.
#ifndef PLACED_PLANT_GLSL
#define PLACED_PLANT_GLSL

// Must match GpuPlacedPlant.
struct PlacedPlant {
    vec3 foot;
    float scale;
    float turnRadians;
    float pick;
    uint kind;
    float threshold;
};

#endif
