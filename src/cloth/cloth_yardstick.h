#pragma once

#include "cloth/cloth.h"
#include "cloth/cloth_collision.h"
#include "core/job_system.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

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
* What the dress-sized test reads off a garment, which has no columns or corners to measure along (docs/CLOTHGPU.md,
* Part 5):
* - meanStretch: the mean of every edge's stretch, length / rest length - 1, a squashed edge counting as none: the
*   garment-wide, low-frequency stretch a solver removes slowly, spread over its edges.
* - percentileStrain: the strain garmentStrainPercentile of the edges are at or below: the stretch where the garment
*   carries its weight, without the odd edge pinched at a seam or a corner of the body.
* - worstEdgeStrain: the most any edge is stretched.
* - worstSeamGap: the widest any stitch stands open, in m (0 once every seam has shut).
* - hemHeight: the hem points' mean height above the floor, in m: a garment that stretches hangs lower.
* - closestToBody: the least signed distance of any point from the field bodies' surfaces, in m, negative inside one
*   (the band where none is near; infinity with no bodies). Contact keeps it at about the cloth's thickness.
*/
inline constexpr float garmentStrainPercentile = 0.99f;

struct GarmentMeasure {
    float meanStretch = 0.0f;
    float percentileStrain = 0.0f;
    float worstEdgeStrain = 0.0f;
    float worstSeamGap = 0.0f;  // m
    float hemHeight = 0.0f;     // m
    float closestToBody = 0.0f; // m
};

// The measures of a garment as it is now, given its hem's points, the floor's height and the bodies it wears.
GarmentMeasure measureGarment(const Cloth& cloth, const std::vector<std::uint32_t>& hem, float floorHeight, const ClothFields& fields = {});

// What a settling does to the cloth before each tick (counted from 0): moving its pins, changing its settings.
using ClothTickPreparation = std::function<void(Cloth& cloth, int tick)>;

/*
* Step the cloth for seconds in ticks of tickSeconds against the field bodies, sharing its colours among the jobs'
* threads, each tick readied by prepare (if given), and return the farthest any point moved over the last simulated
* second (or the whole run, if shorter), in m: how settled it is.
*/
float settleYardstick(Cloth& cloth, float seconds, float tickSeconds, JobSystem& jobs = serialJobs(), const ClothFields& fields = {},
                      const ClothTickPreparation& prepare = {});
