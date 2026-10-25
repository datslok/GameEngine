#pragma once

#include "cloth/cloth.h"
#include "core/job_system.h"

#include <array>

/*
* The yardstick every cloth solver is judged by (docs/CLOTHGPU.md, Part 5): the same 1 m sheet of the same fabric hung
* from its two top corners at four resolutions, settled, then measured. Real cotton barely stretches (about 0.15% under
* its own weight), and a fixed compliance per edge gives the same fabric on any square mesh, so whatever stretch a
* sheet shows beyond that, and whatever differs between its resolutions, is the solver's error, not the fabric's.
*/
constexpr std::array<int, 4> yardstickRungs{25, 50, 100, 200}; // squares a side
constexpr float yardstickSheetSize = 1.0f;                     // m
constexpr float yardstickSettleSeconds = 10.0f;                // simulated

/*
* A yardstickSheetSize sheet of squares x squares hanging upright in the x-y plane, facing +z, at rest, its top edge
* along y = 0 centred on x = 0, pinned at its two top corners (points squares * (squares + 1) and the row's last).
* Throws std::invalid_argument for no squares.
*/
Cloth yardstickSheet(int squares, const ClothSettings& settings);

/*
* What the yardstick reads off a sheet:
* - pathStretch: the longest column from the top edge to the hem against its rest length, minus 1: the sheet-wide,
*   low-frequency stretch that local passes remove slowly.
* - worstEdgeStrain: the most any edge is stretched, length / rest length - 1: the local error. At least pathStretch,
*   since a column's mean strain is never more than its worst edge's.
* - sag: how far the hem's middle (its middle point, or the mean of its two middle points) hangs below the pins. A sheet that cannot stretch hangs it at most yardstickSheetSize,
*   the hem's middle being that far from each pin along the cloth, so more than that is stretch too.
*/
struct SheetMeasure {
    float pathStretch = 0.0f;
    float worstEdgeStrain = 0.0f;
    float sag = 0.0f; // m
};

// The measures of a yardstick sheet of this many squares a side, as it is now.
SheetMeasure measureSheet(const Cloth& cloth, int squares);

/*
* Step the cloth for seconds in ticks of tickSeconds, sharing its colours among the jobs' threads, and return the
* farthest any point moved over the last simulated second (or the whole run, if shorter), in m: how settled it is.
*/
float settleYardstick(Cloth& cloth, float seconds, float tickSeconds, JobSystem& jobs = serialJobs());
