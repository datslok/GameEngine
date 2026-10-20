#include "render/gpu/probe_relocation.h"
#include "engine/debug_views.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/ray_tracing.h"
#include "math/aabb.h"
#include "scene/frame_description.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return nearlyEqual(actual.x, expected.x, tolerance) && nearlyEqual(actual.y, expected.y, tolerance) &&
               nearlyEqual(actual.z, expected.z, tolerance);
    }

    Vec3 offsetOf(const GpuProbeRelocation& relocation) {
        return Vec3{relocation.offset[0], relocation.offset[1], relocation.offset[2]};
    }

    Vec3 offsetOf(const GpuProbeState& state) {
        return Vec3{state.offset[0], state.offset[1], state.offset[2]};
    }

    /*
    * A ray against solid boxes, as the GPU's trace reports it: the nearest face it meets, its distance negative when the
    * ray starts inside a box (it meets that box's inside, a back face), probeRayMiss when it meets nothing.
    */
    float traceBoxes(const std::vector<Aabb>& boxes, const Vec3& origin, const Vec3& direction) {
        float nearest = probeRayMiss;
        bool back = false;

        for (const Aabb& box : boxes) {
            float entry = 0.0f;
            float exit = std::numeric_limits<float>::max();
            bool misses = false;

            for (int axis = 0; axis < 3; ++axis) {
                const float start = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
                const float along = axis == 0 ? direction.x : axis == 1 ? direction.y : direction.z;
                const float low = axis == 0 ? box.minimum.x : axis == 1 ? box.minimum.y : box.minimum.z;
                const float high = axis == 0 ? box.maximum.x : axis == 1 ? box.maximum.y : box.maximum.z;

                if (along == 0.0f) {
                    misses = misses || start < low || start > high;
                    continue;
                }

                const float first = (low - start) / along;
                const float second = (high - start) / along;
                entry = std::max(entry, std::min(first, second));
                exit = std::min(exit, std::max(first, second));
            }

            if (misses || entry > exit) {
                continue;
            }

            const bool inside = box.contains(origin);
            const float distance = inside ? exit : entry;

            if (distance < nearest) {
                nearest = distance;
                back = inside;
            }
        }

        return back ? -nearest : nearest;
    }

    GpuProbeRelocation relocateAmongBoxes(const std::vector<Aabb>& boxes, const Vec3& gridPosition, const Vec3& spacing,
                                          int iterations = probeRelocationIterations) {
        return relocateProbe(gridPosition, spacing, iterations,
                             [&](const Vec3& origin, const Vec3& direction) { return traceBoxes(boxes, origin, direction); });
    }

    bool withinOffsetLimit(const Vec3& offset, const Vec3& spacing) {
        const Vec3 limit = spacing * probeOffsetLimit;
        return std::abs(offset.x) <= limit.x && std::abs(offset.y) <= limit.y && std::abs(offset.z) <= limit.z;
    }

    // A closed room 4 m across and 3 m high on the floor, its 0.3 m walls, floor and ceiling as solid slabs.
    std::vector<Aabb> closedRoom() {
        return {
            Aabb{Vec3{-2.3f, -0.3f, -2.3f}, Vec3{2.3f, 0.0f, 2.3f}},  // floor
            Aabb{Vec3{-2.3f, 3.0f, -2.3f}, Vec3{2.3f, 3.3f, 2.3f}},   // ceiling
            Aabb{Vec3{-2.3f, 0.0f, -2.3f}, Vec3{-2.0f, 3.0f, 2.3f}},  // west wall
            Aabb{Vec3{2.0f, 0.0f, -2.3f}, Vec3{2.3f, 3.0f, 2.3f}},    // east wall
            Aabb{Vec3{-2.0f, 0.0f, -2.3f}, Vec3{2.0f, 3.0f, -2.0f}},  // north wall
            Aabb{Vec3{-2.0f, 0.0f, 2.0f}, Vec3{2.0f, 3.0f, 2.3f}},    // south wall
        };
    }

    /*
    * The largest scale of a step that keeps the offset within the limit on every axis, less the escape margin along it:
    * each axis the step moves along allows (limit - offset) / step towards the side it moves to, and the tightest wins.
    */
    void testEscapeScale() {
        const Vec3 limit{0.4f, 0.4f, 0.4f};
        const float margin = probeEscapeMargin;

        assert(nearlyEqual(probeEscapeScale(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, limit), 4.0f - margin / 0.1f));
        assert(nearlyEqual(probeEscapeScale(Vec3{0.0f, 0.0f, 0.0f}, Vec3{-0.2f, 0.0f, 0.0f}, limit), 2.0f - margin / 0.2f));
        assert(nearlyEqual(probeEscapeScale(Vec3{0.3f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, limit), 1.0f - margin / 0.1f));

        // The tightest axis decides: y may only grow by 0.1 of its 0.2 step.
        const Vec3 step{0.1f, 0.2f, 0.0f};
        assert(nearlyEqual(probeEscapeScale(Vec3{0.0f, 0.3f, 0.0f}, step, limit), 0.5f - margin / step.length()));

        // Already past the limit on an axis it moves further along: no room at all.
        assert(probeEscapeScale(Vec3{0.5f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, limit) < 0.0f);

        // Uneven spacing: each axis has its own limit.
        assert(nearlyEqual(probeEscapeScale(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.5f}, Vec3{0.4f, 0.4f, 1.0f}), 2.0f - margin / 0.5f));
    }

    /*
    * The tally keeps what relocation and classification need of a probe's rays: how many met back faces and front faces,
    * the nearest back face, the nearest and farthest front face, and how far beyond the probe's own cells (one spacing
    * each way round its grid point) the nearest front face hit lies, worst axis first (0 or less: inside them).
    */
    void testRayTally() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const Vec3 start{0.0f, 0.0f, 0.0f};
        ProbeRayTally tally;

        tallyProbeRay(tally, Vec3{1.0f, 0.0f, 0.0f}, 0.5f, start, spacing);
        tallyProbeRay(tally, Vec3{-1.0f, 0.0f, 0.0f}, 3.0f, start, spacing);
        tallyProbeRay(tally, Vec3{0.0f, 1.0f, 0.0f}, -0.2f, start, spacing);
        tallyProbeRay(tally, Vec3{0.0f, -1.0f, 0.0f}, -0.7f, start, spacing);
        tallyProbeRay(tally, Vec3{0.0f, 0.0f, 1.0f}, probeRayMiss, start, spacing);

        assert(tally.rays == 5);
        assert(tally.backFaces == 2);
        assert(tally.frontFaces == 2);
        assert(nearlyEqual(tally.closestBackFace, 0.2f));
        assert(nearlyEqual(tally.closestBackFaceDirection, Vec3{0.0f, 1.0f, 0.0f}));
        assert(nearlyEqual(tally.closestFrontFace, 0.5f));
        assert(nearlyEqual(tally.closestFrontFaceDirection, Vec3{1.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(tally.farthestFrontFace, 3.0f));
        assert(nearlyEqual(tally.farthestFrontFaceDirection, Vec3{-1.0f, 0.0f, 0.0f}));

        // The hit 0.5 m away lies inside the cells (half a spacing in), so the nearest surface is 0.5 inside their edge.
        assert(nearlyEqual(tally.surfaceBeyondCells, -0.5f));

        // Measured from the grid point, not the moved probe: a probe moved 0.3 m along x sees that hit 0.8 m out.
        ProbeRayTally moved;
        tallyProbeRay(moved, Vec3{1.0f, 0.0f, 0.0f}, 0.5f, Vec3{0.3f, 0.0f, 0.0f}, spacing);
        assert(nearlyEqual(moved.surfaceBeyondCells, -0.2f));

        // Nothing in front: no surface anywhere near.
        ProbeRayTally empty;
        tallyProbeRay(empty, Vec3{1.0f, 0.0f, 0.0f}, probeRayMiss, start, spacing);
        assert(empty.frontFaces == 0 && empty.surfaceBeyondCells > 1e30f);
    }

    /*
    * A probe inside a wall (most of its rays meet back faces) moves out through the nearest face, as far beyond it as
    * the offset limit allows, so it ends up in the open on the near side, within its own cell.
    */
    void testBuriedProbeEscapes() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const std::vector<Aabb> wall{Aabb{Vec3{-0.15f, -5.0f, -5.0f}, Vec3{0.15f, 5.0f, 5.0f}}};
        const GpuProbeRelocation relocation = relocateAmongBoxes(wall, Vec3{0.05f, 0.0f, 0.0f}, spacing);
        const Vec3 offset = offsetOf(relocation);

        assert(relocation.off == 0.0f);
        assert(0.05f + offset.x > 0.15f);
        assert(withinOffsetLimit(offset, spacing));
        assert(!wall[0].contains(Vec3{0.05f, 0.0f, 0.0f} + offset));
    }

    // A probe buried deeper than the offset limit reaches cannot get out: it stays where it is and is switched off.
    void testDeeplyBuriedProbeIsOff() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const std::vector<Aabb> block{Aabb{Vec3{-2.0f, -2.0f, -2.0f}, Vec3{2.0f, 2.0f, 2.0f}}};
        const GpuProbeRelocation relocation = relocateAmongBoxes(block, Vec3{0.0f, 0.0f, 0.0f}, spacing);

        assert(relocation.off == 1.0f);
        assert(nearlyEqual(offsetOf(relocation), Vec3{0.0f, 0.0f, 0.0f}));
        assert(classifyProbe(relocation, 0.2f) == ProbeState::Off);
    }

    /*
    * A probe a hair from a wall moves straight away from it, into the room, and stays within its limit.
    */
    void testProbeNearWallMovesAway() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const std::vector<Aabb> room = closedRoom();
        const Vec3 grid{1.95f, 1.5f, 0.3f};
        const GpuProbeRelocation relocation = relocateAmongBoxes(room, grid, spacing);
        const Vec3 moved = grid + offsetOf(relocation);

        assert(relocation.off == 0.0f);
        assert(2.0f - moved.x >= probeMinFrontFaceShare * spacing.x);
        assert(withinOffsetLimit(offsetOf(relocation), spacing));
        assert(classifyProbe(relocation, 0.2f) == ProbeState::Vigilant);
    }

    /*
    * A probe low over a room's floor beside an open doorway, with ground stretching far outside: the farthest surface it
    * sees lies out through the doorway. It rises straight off the floor, the nearest surface, until it stands
    * probeFrontFaceNudge of a spacing above it, and keeps its place across the floor. Moving towards the farthest surface
    * (the 2021 reference's rule) walked it into the doorway, where it saw mostly the sunlit outdoors and lent that to the
    * room's walls (the light lab's back room seen from afar through its door, lit by its one 4 m probe).
    */
    void testProbeLowOverAFloorRisesRatherThanWalkingOut() {
        const Vec3 spacing{2.0f, 2.0f, 2.0f};
        const std::vector<Aabb> room{
            Aabb{Vec3{-2.3f, -0.3f, -2.3f}, Vec3{40.0f, 0.0f, 2.3f}}, // the floor, running far out of the doorway as ground
            Aabb{Vec3{-2.3f, 3.0f, -2.3f}, Vec3{2.3f, 3.3f, 2.3f}},   // ceiling
            Aabb{Vec3{-2.3f, 0.0f, -2.3f}, Vec3{-2.0f, 3.0f, 2.3f}},  // west wall
            Aabb{Vec3{2.0f, 0.0f, -2.3f}, Vec3{2.3f, 3.0f, -0.5f}},   // east wall, either side of the doorway and over it
            Aabb{Vec3{2.0f, 0.0f, 0.5f}, Vec3{2.3f, 3.0f, 2.3f}},
            Aabb{Vec3{2.0f, 2.0f, -0.5f}, Vec3{2.3f, 3.0f, 0.5f}},
            Aabb{Vec3{-2.0f, 0.0f, -2.3f}, Vec3{2.0f, 3.0f, -2.0f}},  // north wall
            Aabb{Vec3{-2.0f, 0.0f, 2.0f}, Vec3{2.0f, 3.0f, 2.3f}},    // south wall
        };
        const Vec3 grid{1.0f, 0.1f, 0.0f};

        // The setting: the floor is too close, and the farthest surface lies out through the doorway.
        ProbeRayTally tally;
        for (int ray = 0; ray < probeRelocationRays; ++ray) {
            const Vec3 direction = sphericalFibonacci(ray, probeRelocationRays);
            tallyProbeRay(tally, direction, traceBoxes(room, grid, direction), Vec3{0.0f, 0.0f, 0.0f}, spacing);
        }
        assert(tally.closestFrontFace < probeMinFrontFaceShare * spacing.y && tally.closestFrontFaceDirection.y < -0.9f);
        assert(tally.farthestFrontFace > 5.0f && tally.farthestFrontFaceDirection.x > 0.9f);

        const GpuProbeRelocation relocation = relocateAmongBoxes(room, grid, spacing);
        const Vec3 offset = offsetOf(relocation);
        assert(relocation.off == 0.0f);
        assert(nearlyEqual(offset.y, probeFrontFaceNudge * spacing.y - grid.y, 0.02f));
        // Its direction is the relocation ray that met the floor nearest, a few degrees off straight down (256 rays on a
        // spiral), so it drifts a few centimetres sideways; walking out it moved 0.8 m towards the doorway.
        assert(std::abs(offset.x) < 0.05f * spacing.x && std::abs(offset.z) < 0.05f * spacing.z);
        assert(withinOffsetLimit(offset, spacing));
    }

    // A probe in the open middle of a room, well away from every surface, stays exactly where it is.
    void testProbeInOpenStays() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const GpuProbeRelocation relocation = relocateAmongBoxes(closedRoom(), Vec3{0.3f, 1.5f, -0.2f}, spacing);

        assert(relocation.off == 0.0f);
        assert(nearlyEqual(offsetOf(relocation), Vec3{0.0f, 0.0f, 0.0f}));
    }

    /*
    * Classification: a probe with a surface within its cells (grown by the self-shadow bias's reach) is vigilant; one with
    * none near sleeps, since no surface ever reads it, and so does one that sees no static surface at all: something
    * that moves may still come near it (the paper switches it off; here only a buried probe is off).
    */
    void testClassification() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const std::vector<Aabb> floor{Aabb{Vec3{-50.0f, -1.0f, -50.0f}, Vec3{50.0f, 0.0f, 50.0f}}};

        const GpuProbeRelocation low = relocateAmongBoxes(floor, Vec3{0.0f, 0.5f, 0.0f}, spacing);
        assert(classifyProbe(low, 0.0f) == ProbeState::Vigilant);

        const GpuProbeRelocation high = relocateAmongBoxes(floor, Vec3{0.0f, 3.5f, 0.0f}, spacing);
        assert(classifyProbe(high, 0.2f) == ProbeState::Sleeping);

        // Just beyond the cells: the bias's reach decides.
        const GpuProbeRelocation edge = relocateAmongBoxes(floor, Vec3{0.0f, 1.1f, 0.0f}, spacing);
        assert(classifyProbe(edge, 0.0f) == ProbeState::Sleeping);
        assert(classifyProbe(edge, 0.2f) == ProbeState::Vigilant);

        const GpuProbeRelocation nothing = relocateAmongBoxes({}, Vec3{0.0f, 0.0f, 0.0f}, spacing);
        assert(nothing.off == 0.0f);
        assert(classifyProbe(nothing, 0.2f) == ProbeState::Sleeping);
    }

    // With no iterations the probe is only classified: never moved, even when buried.
    void testNoIterationsOnlyClassifies() {
        const Vec3 spacing{1.0f, 1.0f, 1.0f};
        const std::vector<Aabb> wall{Aabb{Vec3{-0.15f, -5.0f, -5.0f}, Vec3{0.15f, 5.0f, 5.0f}}};
        const GpuProbeRelocation relocation = relocateAmongBoxes(wall, Vec3{0.05f, 0.0f, 0.0f}, spacing, 0);

        assert(nearlyEqual(offsetOf(relocation), Vec3{0.0f, 0.0f, 0.0f}));
        assert(relocation.off == 1.0f);
    }

    // The bias's reach is the longest the self-shadow bias can be: its normal and view shares both at full length.
    void testBiasReach() {
        const Vec3 spacing{1.0f, 0.5f, 2.0f};
        const float reach = probeBiasReach(spacing, 0.3f);
        assert(nearlyEqual(reach, (biasNormalShare + biasViewShare) * biasSpacingShare * 0.5f * 0.3f));

        const Vec3 along{0.0f, 1.0f, 0.0f};
        assert(probeSelfShadowBias(along, along, spacing, 0.3f).length() <= reach + 1e-6f);
    }

    GpuProbeRelocation relocationRecord(const Vec3& offset, bool off, float surfaceBeyondCells) {
        GpuProbeRelocation record;
        record.offset[0] = offset.x;
        record.offset[1] = offset.y;
        record.offset[2] = offset.z;
        record.off = off ? 1.0f : 0.0f;
        record.surfaceBeyondCells = surfaceBeyondCells;
        return record;
    }

    /*
    * Each frame's states: before any relocation every probe is traced where the grid puts it (as before relocation
    * existed); after, the relocation's offsets and classes, and a sleeping probe near something that moves wakes, while
    * an off probe (buried in the static level) never does.
    */
    void testProbeStates() {
        const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{8, 1, 1}};
        GlobalIlluminationSettings settings;

        const std::vector<GpuProbeState> before = buildProbeStates(volume, {}, {}, settings);
        assert(before.size() == 8);

        for (const GpuProbeState& state : before) {
            assert(state.state == static_cast<std::int32_t>(ProbeState::Vigilant));
            assert(nearlyEqual(offsetOf(state), Vec3{0.0f, 0.0f, 0.0f}));
        }

        std::vector<GpuProbeRelocation> relocations;

        for (int probe = 0; probe < 8; ++probe) {
            relocations.push_back(relocationRecord(Vec3{0.0f, 0.0f, 0.0f}, false, 5.0f)); // nothing near: sleeping
        }

        relocations[0] = relocationRecord(Vec3{0.1f, 0.0f, 0.0f}, false, -0.5f); // vigilant, moved
        relocations[7] = relocationRecord(Vec3{0.0f, 0.0f, 0.0f}, true, 5.0f);   // off

        const std::vector<GpuProbeState> still = buildProbeStates(volume, relocations, {}, settings);
        assert(still[0].state == static_cast<std::int32_t>(ProbeState::Vigilant));
        assert(nearlyEqual(offsetOf(still[0]), Vec3{0.1f, 0.0f, 0.0f}));
        assert(still[3].state == static_cast<std::int32_t>(ProbeState::Sleeping));
        assert(still[7].state == static_cast<std::int32_t>(ProbeState::Off));

        // A ball of radius 0.25 at x = 5: the probes whose cells, grown by the bias's reach and the wake margin, it touches.
        // At the default 1 cell of margin that is two cells and the reach each way: x = 3..7, but 7 is off.
        const std::vector<BoundingSphere> movers{BoundingSphere{Vec3{5.0f, 0.0f, 0.0f}, 0.25f}};
        const std::vector<GpuProbeState> woken = buildProbeStates(volume, relocations, movers, settings);
        assert(woken[2].state == static_cast<std::int32_t>(ProbeState::Sleeping));

        for (int probe = 3; probe <= 6; ++probe) {
            assert(woken[probe].state == static_cast<std::int32_t>(ProbeState::Awake));
        }

        assert(woken[7].state == static_cast<std::int32_t>(ProbeState::Off));

        // Without a wake margin only the cells round the ball itself: x = 4..6.
        GlobalIlluminationSettings tight = settings;
        tight.wakeCells = 0.0f;
        const std::vector<GpuProbeState> near = buildProbeStates(volume, relocations, movers, tight);
        assert(near[3].state == static_cast<std::int32_t>(ProbeState::Sleeping));
        assert(near[4].state == static_cast<std::int32_t>(ProbeState::Awake) && near[6].state == static_cast<std::int32_t>(ProbeState::Awake));

        // A draw with no bounds yet wakes nothing.
        const std::vector<BoundingSphere> unknown{BoundingSphere{Vec3{5.0f, 0.0f, 0.0f}, -1.0f}};
        assert(buildProbeStates(volume, relocations, unknown, settings)[5].state == static_cast<std::int32_t>(ProbeState::Sleeping));

        // States off: every probe traced, moved still. Relocation off: none moved, still classified.
        GlobalIlluminationSettings noStates = settings;
        noStates.probeStates = false;
        const std::vector<GpuProbeState> allTraced = buildProbeStates(volume, relocations, {}, noStates);
        assert(allTraced[7].state == static_cast<std::int32_t>(ProbeState::Vigilant));
        assert(nearlyEqual(offsetOf(allTraced[0]), Vec3{0.1f, 0.0f, 0.0f}));

        GlobalIlluminationSettings unmoved = settings;
        unmoved.probeRelocation = false;
        const std::vector<GpuProbeState> inPlace = buildProbeStates(volume, relocations, {}, unmoved);
        assert(nearlyEqual(offsetOf(inPlace[0]), Vec3{0.0f, 0.0f, 0.0f}));
        assert(inPlace[3].state == static_cast<std::int32_t>(ProbeState::Sleeping));
    }

    // Only awake and vigilant probes are traced, updated and read.
    void testTracedStates() {
        assert(!probeTraced(ProbeState::Off));
        assert(!probeTraced(ProbeState::Sleeping));
        assert(probeTraced(ProbeState::Awake));
        assert(probeTraced(ProbeState::Vigilant));
    }

    GpuProbeState stateRecord(ProbeState state, const Vec3& offset = Vec3{0.0f, 0.0f, 0.0f}) {
        return GpuProbeState{{offset.x, offset.y, offset.z}, static_cast<std::int32_t>(state)};
    }

    /*
    * A probe that starts being traced (woken, or newly vigilant) or is moved holds light from another time or place, so
    * it restarts: the probes that did either between two frames' states. A different size is a new volume, whose
    * atlases start fresh anyway: none.
    */
    void testRestartedProbes() {
        const std::vector<GpuProbeState> before{stateRecord(ProbeState::Sleeping), stateRecord(ProbeState::Vigilant), stateRecord(ProbeState::Awake),
                                                stateRecord(ProbeState::Off), stateRecord(ProbeState::Vigilant)};
        const std::vector<GpuProbeState> after{stateRecord(ProbeState::Awake), stateRecord(ProbeState::Vigilant), stateRecord(ProbeState::Sleeping),
                                               stateRecord(ProbeState::Off), stateRecord(ProbeState::Vigilant, Vec3{0.1f, 0.0f, 0.0f})};

        const std::vector<int> restarted = restartedProbes(before, after);
        assert((restarted == std::vector<int>{0, 4}));
        assert(restartedProbes(before, std::vector<GpuProbeState>(3)).empty());
    }

    /*
    * A restarted probe starts outright on its first frame: no share of its stale light kept, in either atlas, and every
    * texel restarts (no event check: its stored light means nothing here), then follows a restart's growing memory.
    */
    void testWokenProbesRestartOutright() {
        const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{3, 1, 1}};
        ProbeFrame frame;
        frame.irradianceHysteresis = 0.98f;
        frame.fastIrradianceHysteresis = 0.8f;
        frame.distanceHysteresis = 0.97f;
        frame.restartHysteresis = 0.5f;
        frame.restartGrowth = 0.5f;
        frame.restartThreshold = 4.0f;
        frame.restartCheckFrames = 16.0f;

        std::vector<GpuProbeRestart> restarts;
        const std::vector<int> woken{1};
        advanceProbeRestarts(restarts, volume, {}, frame, woken);

        assert(restarts[1].irradianceFrames == 0 && restarts[1].distanceFrames == 0);
        assert(restarts[1].irradianceKeep == 0.0f && restarts[1].distanceKeep == 0.0f && restarts[1].fastKeep == 0.0f);
        assert(restarts[1].check.threshold == 0.0f);
        assert(restarts[0].irradianceFrames < 0 && nearlyEqual(restarts[0].irradianceKeep, 0.98f));
        assert(restarts[0].check.threshold == 4.0f);

        advanceProbeRestarts(restarts, volume, {}, frame);
        assert(restarts[1].irradianceFrames == 1);
        assert(restarts[1].irradianceKeep > 0.0f && restarts[1].check.threshold == 4.0f);
    }

    /*
    * Relocation sees only the static level: a static shadow caster carries the static bit as well as the solid one, so
    * probe rays (solid) still see it and relocation's rays (static) see nothing else.
    */
    void testStaticRayMask() {
        DrawItem wall;
        wall.isStatic = true;
        assert(rayMaskFor(wall) == (solidRayMask | staticRayMask));

        DrawItem ball;
        assert(rayMaskFor(ball) == solidRayMask);

        DrawItem bulb;
        bulb.isStatic = true;
        bulb.castsShadows = false;
        assert(rayMaskFor(bulb) == visualOnlyRayMask);

        assert((staticRayMask & solidRayMask) == 0 && (staticRayMask & visualOnlyRayMask) == 0);
    }

    // What moves: every draw that is not static and has its bounds.
    void testMovingBounds() {
        DrawItem wall;
        wall.isStatic = true;
        wall.bounds = BoundingSphere{Vec3{1.0f, 0.0f, 0.0f}, 2.0f};
        DrawItem ball;
        ball.bounds = BoundingSphere{Vec3{3.0f, 0.0f, 0.0f}, 0.5f};
        DrawItem unbounded;

        const std::vector<DrawItem> draws{wall, ball, unbounded};
        const std::vector<BoundingSphere> movers = movingBounds(draws);
        assert(movers.size() == 1);
        assert(nearlyEqual(movers[0].centre, Vec3{3.0f, 0.0f, 0.0f}) && movers[0].radius == 0.5f);
    }

    // The F4 readout's probe line: how many probes, traced (awake ones among them), asleep, off and moved.
    void testProbeCounts() {
        const std::vector<GpuProbeState> states{stateRecord(ProbeState::Vigilant), stateRecord(ProbeState::Vigilant, Vec3{0.1f, 0.0f, 0.0f}),
                                                stateRecord(ProbeState::Awake), stateRecord(ProbeState::Sleeping),
                                                stateRecord(ProbeState::Off, Vec3{0.0f, 0.0f, -0.2f})};
        const ProbeStateCounts counts = countProbeStates(states);
        assert(counts.probes == 5 && counts.traced == 3 && counts.awake == 1 && counts.sleeping == 1 && counts.off == 1 && counts.moved == 2);

        assert(probeStatsText(counts) == "PROBES 5  TRACED 3  AWAKE 1  ASLEEP 1  OFF 1  MOVED 2");
        assert(probeStatsText(ProbeStateCounts{}) == "PROBES -");
    }
}

void testProbeRelocation() {
    testEscapeScale();
    testRayTally();
    testBuriedProbeEscapes();
    testDeeplyBuriedProbeIsOff();
    testProbeNearWallMovesAway();
    testProbeLowOverAFloorRisesRatherThanWalkingOut();
    testProbeInOpenStays();
    testClassification();
    testNoIterationsOnlyClassifies();
    testBiasReach();
    testProbeStates();
    testTracedStates();
    testRestartedProbes();
    testWokenProbesRestartOutright();
    testStaticRayMask();
    testMovingBounds();
    testProbeCounts();
}
