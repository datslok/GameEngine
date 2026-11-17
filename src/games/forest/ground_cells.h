#pragma once

#include "core/job_system.h"
#include "math/vec2.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The cells ground cover is drawn in (docs/research/GROUNDCOVER.md): squares of the land, each drawing one of a few shared
* patches the vertex shaders lift onto it (ModelRenderer::followsGround), as dense as one of the ground's paint channels
* there. The grass reads the grass paint (games/forest/grass.h), the forest floor's cover the leaf litter's
* (games/forest/floor_cover.h), so each grows exactly where the ground is painted as it.
*/

// A cell: its middle on the ground's plane, its density step (1 to the steps), its patch variant and its quarter turns.
struct GroundCell {
    Vec2 centre;
    int step = 0;
    int variant = 0;
    int quarterTurns = 0;
};

// What chooses the cells: their side, how far from the path's centre line they reach, the least mean paint drawing
// anything, the density steps and patch variants, the paint channel (forestGroundPaint's) and the seed for variants and
// turns.
struct GroundCellOptions {
    float cellMetres = 8.0f;
    float reachMetres = 200.0f;
    float leastPaint = 0.05f;
    int densitySteps = 4;
    int variants = 4;
    std::size_t layer = 0;
    std::uint64_t seed = 0;
};

// A mean paint's density step: 0 (nothing) below leastPaint, else the nearest of 1 to steps.
int groundDensityStep(int steps, float leastPaint, float paint);

/*
* A grid of cellMetres squares aligned on the origin, those within reachMetres of the path, each with its layer's mean
* paint (forestGroundPaint at 4 x 4 points over it) stepped; cells stepping to 0 left out. Its variant and turn from its
* place and the seed. A row of cells a job; the same every run.
*/
std::vector<GroundCell> forestGroundCells(const GroundCellOptions& options, JobSystem& jobs);

// A level's error for the distance it should start at (metres): what covers the levels' default pixel error there at a
// 1080-pixel, 70 degree view.
float groundCoverLevelError(float distance);
