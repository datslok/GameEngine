#pragma once

/*
* How much of a light reaches a point: the CPU mirror of lighting.glsl's distanceFalloff and spotCone, so tests can check
* the shapes, and the GPU tests (tests/gpu) can check the shader against them.
*/

// The window that ends a light at its range: (1 - (distance / range)^4) clamped to 0..1. range must be positive.
float rangeWindow(float distance, float range);

/*
* Inverse square falloff softened near the light by the size of its source, 1 / (distance^2 + sourceRadius^2), times
* the window (1 - (distance / range)^4)^2, clamped to 0 beyond the range, so the light ends exactly at its range with
* no visible edge. range must be positive.
*/
float distanceFalloff(float distance, float range, float sourceRadius);

/*
* A spotlight's soft edge from the cosine of the angle between the beam's axis and the way to the point: 0 outside the
* outer cone, 1 inside the inner one, GLSL's smoothstep (3t^2 - 2t^3) between. cosInner must be above cosOuter.
*/
float spotCone(float alongBeam, float cosOuter, float cosInner);
