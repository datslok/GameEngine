#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_particles.h"
#include "cloth/cloth_tethers.h"
#include "math/vec3.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* Sewing. A seam joins the edges of two pieces of fabric, point by point: each pair is a stitch, two points that end up
* as one. Garment software sews panels laid round a body by pulling every stitch shut at once; so does this.
*
* A stitch is a weld whose gap closes at a set speed: after each substep its gap may be no wider than it was when the
* substep began less a step,
*
*   d <= max(0, d_start - closingStep),     closingStep = seam speed x substep,
*
* and a wider gap is pulled in to that, shared by inverse mass as any constraint is (the heavier point moving less). A
* zero-length XPBD constraint would shut a 10 cm gap in one substep and leave the points flying apart at 200 m/s; this
* one shuts it in a tenth of a second at 1 m/s. The limit is measured from the substep's start, not from where the
* points have got to, because the pull becomes the points' velocity (it is worked out from their motion): pulling a step
* from wherever they are would add a step of speed every substep, a 1,800 m/s2 acceleration, and the panels would slam
* together and bounce. Measured from the start, once the points close at the seam speed by themselves the stitch has
* nothing to add; something closing them faster is left alone. Once shut it is rigid: nothing else moves a point
* anywhere near closingStep (0.5 mm) in a substep, and it holds no state of its own, so nothing changes from tick to tick.
*/
struct ClothStitch {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
};

/*
* The widest a stitch's gap may be at the end of this substep: its width at the substep's start (the points' previous
* positions) less closingStep (metres). Nothing or less means the stitch shuts, or is shut; more means it is still
* being sewn (mirrored by cloth.glsl's clothStitchWidest).
*/
inline float stitchWidest(const Vec3& firstPrevious, const Vec3& secondPrevious, float closingStep) {
    return (secondPrevious - firstPrevious).length() - closingStep;
}

/*
* One stitch (solveStitches' step, mirrored by cloth.glsl's clothStitch): its gap pulled in to its width at the
* substep's start (the points' previous positions) less closingStep (metres); once that is nothing, both points land on
* the same spot to the last bit, the point where their inverse masses balance. Two pinned points stay as they are.
*/
inline void solveStitch(Vec3& first, Vec3& second, float firstWeight, float secondWeight, const Vec3& firstPrevious, const Vec3& secondPrevious,
                        float closingStep) {
    const float weights = firstWeight + secondWeight;
    if (weights <= 0.0f) {
        return;
    }

    const float widest = stitchWidest(firstPrevious, secondPrevious, closingStep);
    const Vec3 gap = second - first;
    if (widest <= 0.0f) {
        const Vec3 joined = first + gap * (firstWeight / weights);
        first = joined;
        second = joined;
        return;
    }

    const float distance = gap.length();
    if (distance <= widest) {
        return;
    }
    const Vec3 move = gap * ((distance - widest) / (distance * weights));
    first = first + move * firstWeight;
    second = second - move * secondWeight;
}

/*
* One pass over the stitches in order. Each point is in one stitch at most (meshPattern makes sure), so no two touch
* the same point and any order, or all at once (the GPU's stitch pass), gives the same result.
*/
void solveStitches(ClothParticles& particles, const std::vector<ClothStitch>& stitches, float closingStep);

/*
* Which points are being sewn this substep (1) and which not (0), one byte a point in sewing (resized to the points):
* both points of each stitch still open, its stitchWidest above nothing. The bodies push such points out without
* friction (solveContacts): a stitch pulls its points along a chord, and where the chord runs into a body (two pieces
* round a shoulder, an armpit) the pull is mostly into it and only a little along it, so static friction, which holds
* a slide under 0.6 of the push, would hold the points where they lie and the seam would never shut. Garment software
* sews the same way, letting the panels slide over the body while the seams pull them together; once shut, a seam's
* points take friction as any other.
*/
void markSewing(const ClothParticles& particles, const std::vector<ClothStitch>& stitches, float closingStep, std::vector<std::uint8_t>& sewing);

/*
* Bending across a seam, so a sewn seam bends like the fabric round it rather than folding as a free hinge: for each
* two stitches in a row along the seam (seam in order along its first edge), a hinge on the first edge's segment
* between them, its wings the far corners of the triangle on each side (one in each piece). Its rest shape is the two
* triangles unfolded flat across the segment, the second piece's corner at its own distance along and across its
* segment, on the side away from the first's: fabric sewn flat rests flat, as a seam pressed open does. Segments with no
* triangle on a side are skipped. triangles are the cloth's, restPositions its rest shape.
*/
std::vector<BendConstraint> seamHinges(const std::vector<Vec3>& restPositions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                       const std::vector<ClothStitch>& seam);

// What a cloth sewn from pieces has besides its mesh: its stitches, the hinges across its seams, and its pieces.
struct ClothSewing {
    std::vector<ClothStitch> stitches;
    std::vector<BendConstraint> hinges;
    ClothPieces pieces;
};
