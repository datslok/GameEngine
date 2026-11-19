#ifndef BEACON_SPLAT_SHARED_H
#define BEACON_SPLAT_SHARED_H

// Numbers the beacons' splat (render/gpu/beacon_splat.h) and its shaders (beacon_splat.glsl) share; plain #defines only.

// Nearer than this (metres) a beacon is taken to be this far, so a camera inside a lamp divides by no zero.
#define BEACON_NEAREST_DISTANCE 0.001

#endif
