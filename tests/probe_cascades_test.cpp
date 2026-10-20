#include "render/gpu/probe_cascades.h"
#include "math/angles.h"
#include "probe_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return nearlyEqual(actual.x, expected.x, tolerance) && nearlyEqual(actual.y, expected.y, tolerance) &&
               nearlyEqual(actual.z, expected.z, tolerance);
    }

    int axisOf(const ProbeGridCoord& grid, int axis) {
        return axis == 0 ? grid.x : axis == 1 ? grid.y : grid.z;
    }

    int axisOf(const ProbeCounts& counts, int axis) {
        return axis == 0 ? counts.x : axis == 1 ? counts.y : counts.z;
    }

    float axisOf(const Vec3& value, int axis) {
        return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
    }

    // Numbers spread over -range..range, the same every run (a fixed linear congruential sequence).
    struct Spread {
        std::uint32_t state = 12345u;

        float next(float range) {
            state = state * 1664525u + 1013904223u;
            return (static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f) * range;
        }

        Vec3 nextVec3(float range) {
            const float x = next(range);
            const float y = next(range);
            return Vec3{x, y, next(range)};
        }
    };

    // The settings' layout: 21 x 13 x 21 probes a cascade, four cascades from half a metre, each twice as far apart.
    void testTheLayoutFollowsTheSettings() {
        const GlobalIlluminationSettings settings;
        const ProbeCascadeLayout layout = probeCascadeLayout(settings);
        assert(layout.counts.x == 21 && layout.counts.y == 13 && layout.counts.z == 21);
        assert(layout.cascades == 4 && layout.spacing == 0.5f);
        assert(settings.seedProbes);
        assert(probeCascadeSpacing(layout, 0) == 0.5f && probeCascadeSpacing(layout, 1) == 1.0f && probeCascadeSpacing(layout, 3) == 4.0f);

        // Kept to what the atlases hold: no more cascades than the most, no more probes than one workgroup each.
        GlobalIlluminationSettings many = settings;
        many.probeCascades = 20;
        assert(probeCascadeLayout(many).cascades == maxProbeCascades);

        GlobalIlluminationSettings huge = settings;
        huge.cascadeProbesAcross = 41;
        huge.cascadeProbesHigh = 21;
        const ProbeCascadeLayout fitted = probeCascadeLayout(huge);
        assert(fitted.cascades >= 1 && probeCount(fitted.counts) * fitted.cascades <= maxProbesPerVolume);
        assert(probeCount(fitted.counts) * (fitted.cascades + 1) > maxProbesPerVolume);

        GlobalIlluminationSettings tiny = settings;
        tiny.cascadeProbesAcross = 0;
        tiny.cascadeProbesHigh = -3;
        tiny.probeCascades = 0;
        tiny.probeSpacing = 0.0f;
        const ProbeCascadeLayout least = probeCascadeLayout(tiny);
        assert(least.counts.x == 2 && least.counts.y == 2 && least.counts.z == 2 && least.cascades == 1 && least.spacing > 0.0f);
    }

    /*
    * A scrolled volume keeps grid place l in slot (l + scroll) mod count, and slots map back to their places; with no
    * scroll, slot and place are the same numbering as before. Places past either end wrap round.
    */
    void testSlotsWrapRound() {
        ProbeVolume volume{Vec3{-2.0f, 0.5f, 3.0f}, Vec3{1.0f, 0.5f, 2.0f}, ProbeCounts{4, 3, 5}};
        assert(probeSlot(volume, ProbeGridCoord{3, 2, 4}) == probeIndex(volume.counts, 3, 2, 4));

        volume.scroll = ProbeScroll{1, 2, 4};
        assert(probeSlot(volume, ProbeGridCoord{0, 0, 0}) == probeIndex(volume.counts, 1, 2, 4));
        assert(probeSlot(volume, ProbeGridCoord{3, 1, 1}) == probeIndex(volume.counts, 0, 0, 0));
        assert(probeSlot(volume, ProbeGridCoord{-1, 0, 0}) == probeSlot(volume, ProbeGridCoord{3, 0, 0}));

        std::set<int> slots;

        for (int slot = 0; slot < probeCount(volume.counts); ++slot) {
            const ProbeGridCoord grid = probeGridCoord(volume, slot);
            assert(grid.x >= 0 && grid.x < 4 && grid.y >= 0 && grid.y < 3 && grid.z >= 0 && grid.z < 5);
            assert(probeSlot(volume, grid) == slot);
            slots.insert(slot);

            const Vec3 expected = volume.origin + Vec3{static_cast<float>(grid.x) * 1.0f, static_cast<float>(grid.y) * 0.5f, static_cast<float>(grid.z) * 2.0f};
            assert(nearlyEqual(probePosition(volume, slot), expected));
        }

        assert(slots.size() == 60);
    }

    /*
    * A window sits on its spacing's lattice from the world's origin, its middle probe the lattice point nearest the centre
    * (so within half a cell of it), its storage scrolled by its first place, so each lattice point has one slot wherever
    * the window is.
    */
    void testAWindowIsCentredOnTheLattice() {
        Spread spread;
        const ProbeCounts counts{21, 13, 20};

        for (int i = 0; i < 2000; ++i) {
            const Vec3 centre = spread.nextVec3(300.0f);
            const ProbeCascade cascade = probeWindow(centre, 0.5f, counts, 0.3f, 7);
            assert(cascade.firstProbe == 7);
            assert(cascade.volume.spacing.x == 0.5f && cascade.volume.spacing.y == 0.5f && cascade.volume.spacing.z == 0.5f);

            for (int axis = 0; axis < 3; ++axis) {
                const int count = axisOf(counts, axis);
                const int start = axisOf(cascade.start, axis);
                assert(start == probeWindowStart(axisOf(centre, axis), 0.5f, count));
                assert(nearlyEqual(axisOf(cascade.volume.origin, axis), static_cast<float>(start) * 0.5f, 1e-4f));

                const float middle = static_cast<float>(start + (count - 1) / 2) * 0.5f;
                assert(std::abs(middle - axisOf(centre, axis)) <= 0.25f + 1e-4f);

                const int scroll = axis == 0 ? cascade.volume.scroll.x : axis == 1 ? cascade.volume.scroll.y : cascade.volume.scroll.z;
                assert(scroll == ((start % count) + count) % count);
            }

            assert(nearlyEqual(cascade.weightCentre, centre));
        }

        // Every slot holds the lattice point its grid place is at.
        const ProbeCascade cascade = probeWindow(Vec3{-13.2f, 4.0f, 101.7f}, 1.0f, ProbeCounts{5, 3, 4}, 0.3f, 0);

        for (int slot = 0; slot < probeCount(cascade.volume.counts); ++slot) {
            const ProbeGridCoord grid = probeGridCoord(cascade.volume, slot);
            const ProbeGridCoord place = probeLatticeCoord(cascade, slot);
            assert(place.x == cascade.start.x + grid.x && place.y == cascade.start.y + grid.y && place.z == cascade.start.z + grid.z);
        }
    }

    /*
    * Moving a cell along an axis brings in exactly one plane: the slots that held the plane left behind now hold the new
    * one in front, and every other slot keeps its lattice point (and so its light). Moving within a cell brings in nothing;
    * a jump further than the window brings in everything.
    */
    void testAScrollBringsInOnePlane() {
        const ProbeCounts counts{9, 5, 7};
        const ProbeCascade before = probeWindow(Vec3{10.1f, 2.0f, -3.0f}, 1.0f, counts, 0.3f, 100);
        const ProbeCascade still = probeWindow(Vec3{10.4f, 2.3f, -3.4f}, 1.0f, counts, 0.3f, 100);
        assert(scrolledProbes(before, still).empty());

        const ProbeCascade after = probeWindow(Vec3{11.1f, 2.0f, -3.0f}, 1.0f, counts, 0.3f, 100);
        assert(after.start.x == before.start.x + 1);
        const std::vector<int> scrolled = scrolledProbes(before, after);
        assert(scrolled.size() == static_cast<std::size_t>(counts.y * counts.z));

        const std::set<int> brought(scrolled.begin(), scrolled.end());

        for (int slot = 0; slot < probeCount(counts); ++slot) {
            const ProbeGridCoord was = probeLatticeCoord(before, slot);
            const ProbeGridCoord now = probeLatticeCoord(after, slot);

            if (brought.count(100 + slot) != 0) {
                assert(was.x == before.start.x && now.x == after.start.x + counts.x - 1);
            } else {
                assert(was.x == now.x && was.y == now.y && was.z == now.z);
            }
        }

        // Back again: the other side's plane comes in.
        const std::vector<int> back = scrolledProbes(after, before);
        assert(back.size() == scrolled.size());

        // A diagonal move of a cell on two axes brings in two planes, sharing a row.
        const ProbeCascade diagonal = probeWindow(Vec3{11.1f, 3.0f, -3.0f}, 1.0f, counts, 0.3f, 100);
        assert(scrolledProbes(before, diagonal).size() == static_cast<std::size_t>(counts.y * counts.z + counts.x * counts.z - counts.z));

        // A jump past the whole window, or another size or spacing: everything is new.
        const ProbeCascade far = probeWindow(Vec3{40.0f, 2.0f, -3.0f}, 1.0f, counts, 0.3f, 100);
        assert(scrolledProbes(before, far).size() == static_cast<std::size_t>(probeCount(counts)));
        const ProbeCascade coarser = probeWindow(Vec3{10.1f, 2.0f, -3.0f}, 2.0f, counts, 0.3f, 100);
        assert(scrolledProbes(before, coarser).size() == static_cast<std::size_t>(probeCount(counts)));
    }

    // The cascades double their spacing, number their probes one after another and are all weighted round the centre.
    void testCascadesDoubleAndNest() {
        const GlobalIlluminationSettings settings;
        const ProbeCascadeLayout layout = probeCascadeLayout(settings);
        const Vec3 centre{-23.7f, 1.6f, 8.9f};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(centre, layout, settings.selfShadowBias);
        assert(cascades.size() == 4);

        for (std::size_t c = 0; c < cascades.size(); ++c) {
            const float spacing = probeCascadeSpacing(layout, static_cast<int>(c));
            assert(cascades[c].volume.spacing.x == spacing);
            assert(cascades[c].firstProbe == static_cast<int>(c) * probeCount(layout.counts));
            assert(nearlyEqual(cascades[c].weightCentre, centre));

            // Each reaches about twice as far as the one inside it.
            if (c > 0) {
                assert(cascades[c].weightReach.x > 1.9f * cascades[c - 1].weightReach.x);
            }
        }

        // The densest lights 3.6 m round the camera at full weight and 1.6 m up and down (the settings' comment).
        const ProbeCascade& densest = cascades.front();
        assert(nearlyEqual(densest.weightReach.x - densest.weightFade.x, 3.6375f, 1e-3f));
        assert(nearlyEqual(densest.weightReach.y - densest.weightFade.y, 1.6375f, 1e-3f));
    }

    /*
    * The point of the weight's margin: wherever the centre is, a point the cascade lights at all, moved by any self-shadow
    * bias up to the setting's reach, falls in a cell whose corners all lie at least one plane inside the window, so a
    * plane a scroll has just brought in is never read.
    */
    void testTheWeightNeverReadsAnOuterPlane() {
        Spread spread;
        const ProbeCounts counts{21, 13, 20};
        const float bias = 0.3f;
        const float spacing = 0.5f;
        const float biasReach = probeBiasReach(Vec3{spacing, spacing, spacing}, bias);
        int lit = 0;

        for (int i = 0; i < 20000; ++i) {
            const Vec3 centre = spread.nextVec3(50.0f);
            const ProbeCascade cascade = probeWindow(centre, spacing, counts, bias, 0);
            const Vec3 point = centre + spread.nextVec3(5.0f);

            if (probeCascadeWeight(cascade, point) <= 0.0f) {
                continue;
            }

            ++lit;
            const Vec3 push = spread.nextVec3(1.0f).normalized() * biasReach;
            const ProbeCell cell = probeCell(cascade.volume, point + push);

            for (int corner = 0; corner < 8; ++corner) {
                const ProbeGridCoord grid = probeCellCorner(cell, counts, corner);

                for (int axis = 0; axis < 3; ++axis) {
                    assert(axisOf(grid, axis) >= 1 && axisOf(grid, axis) <= axisOf(counts, axis) - 2);
                }
            }
        }

        assert(lit > 2000);
    }

    // The weight: full near the centre, half way through the fade, nothing past the reach, a product over the axes.
    void testTheWeightFadesRoundTheCentre() {
        const ProbeCascade cascade = probeWindow(Vec3{1.0f, 2.0f, 3.0f}, 1.0f, ProbeCounts{21, 13, 21}, 0.3f, 0);
        const float reach = probeCascadeReach(21, 1.0f, probeBiasReach(Vec3{1.0f, 1.0f, 1.0f}, 0.3f));
        assert(nearlyEqual(cascade.weightReach.x, reach) && nearlyEqual(cascade.weightFade.x, probeCascadeFadeCells));
        assert(nearlyEqual(reach, (10.0f - 0.5f - probeCascadeEdgeCells) - 0.225f));

        assert(probeCascadeWeight(cascade, Vec3{1.0f, 2.0f, 3.0f}) == 1.0f);
        assert(probeCascadeWeight(cascade, Vec3{1.0f - (reach - 1.0f), 2.0f, 3.0f}) == 1.0f);
        assert(nearlyEqual(probeCascadeWeight(cascade, Vec3{1.0f + reach - 0.5f, 2.0f, 3.0f}), 0.5f));
        assert(nearlyEqual(probeCascadeWeight(cascade, Vec3{1.0f + reach - 0.5f, 2.0f, 3.0f - (reach - 0.5f)}), 0.25f));
        assert(probeCascadeWeight(cascade, Vec3{1.0f + reach, 2.0f, 3.0f}) == 0.0f);
        assert(probeCascadeWeight(cascade, Vec3{1.0f, 2.0f + 40.0f, 3.0f}) == 0.0f);
    }

    // A fixed volume keeps the probes' first rule: full out to half a cell beyond its outer probes, then a half-cell fade.
    void testAFixedVolumeFadesBeyondItsProbes() {
        const ProbeCascade unit = fixedProbeCascade(ProbeVolume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{4, 4, 4}});

        // Probes from 0 to 3 on each axis.
        assert(probeCascadeWeight(unit, Vec3{1.5f, 1.5f, 1.5f}) == 1.0f);
        assert(probeCascadeWeight(unit, Vec3{0.0f, 3.0f, 1.0f}) == 1.0f);

        // Full out to the margin, half way through the fade, nothing past it.
        assert(probeCascadeWeight(unit, Vec3{-probeVolumeMarginCells, 1.0f, 1.0f}) == 1.0f);
        const float halfway = probeVolumeMarginCells + 0.5f * probeVolumeFadeCells;
        assert(nearlyEqual(probeCascadeWeight(unit, Vec3{3.0f + halfway, 1.0f, 1.0f}), 0.5f));
        assert(nearlyEqual(probeCascadeWeight(unit, Vec3{-halfway, 1.0f, 3.0f + halfway}), 0.25f));
        assert(probeCascadeWeight(unit, Vec3{1.0f, -5.0f, 1.0f}) == 0.0f);

        // Spacing scales the margin: with 2 m between probes, a point 1 m outside is still fully in.
        const ProbeCascade wide = fixedProbeCascade(ProbeVolume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f}, ProbeCounts{2, 2, 2}});
        assert(probeCascadeWeight(wide, Vec3{1.0f, -1.0f, 1.0f}) == 1.0f);

        // A single layer of probes reaches the margin either side of it.
        const ProbeCascade layer = fixedProbeCascade(ProbeVolume{Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{3, 1, 3}});
        assert(probeCascadeWeight(layer, Vec3{1.0f, 1.0f - probeVolumeMarginCells, 1.0f}) == 1.0f);
        assert(fixedProbeCascade(layer.volume, 9).firstProbe == 9);
    }

    // A lookup whose every probe of cascade c holds light[c] in every direction, all traced, distances far.
    ProbeLookup cascadeLookup(const std::vector<ProbeCascade>& cascades, const std::vector<Vec3>& light, const Vec3& ambient,
                              int untracedCascade = -1) {
        const auto cascadeOf = [cascades](int probe) {
            int c = 0;

            while (c + 1 < static_cast<int>(cascades.size()) && probe >= cascades[static_cast<std::size_t>(c + 1)].firstProbe) {
                ++c;
            }

            return c;
        };

        ProbeLookup lookup;
        lookup.irradiance = [=](int probe, const Vec3&) { return light[static_cast<std::size_t>(cascadeOf(probe))]; };
        lookup.fastIrradiance = [=](int probe, const Vec3&) { return light[static_cast<std::size_t>(cascadeOf(probe))] * 0.5f; };
        lookup.moments = [](int, const Vec3&) { return Vec2{100.0f, 10000.0f}; };
        lookup.state = [=](int probe) {
            GpuProbeState state;
            state.state = static_cast<std::int32_t>(cascadeOf(probe) == untracedCascade ? ProbeState::Sleeping : ProbeState::Vigilant);
            return state;
        };
        lookup.ambient = ambient;
        return lookup;
    }

    /*
    * The densest cascade lights what it reaches; across its fade the next one takes the rest, and past the coarsest the
    * flat ambient does (fast light: nothing). A cascade with no traced probes round a point leaves it to the coarser.
    */
    void testCascadesBlendDensestFirst() {
        const ProbeCascadeLayout layout{ProbeCounts{9, 9, 9}, 1.0f, 2};
        const Vec3 centre{0.2f, 0.1f, -0.3f};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(centre, layout, 0.3f);
        const std::vector<Vec3> light{Vec3{1.0f, 1.0f, 1.0f}, Vec3{4.0f, 4.0f, 4.0f}};
        const Vec3 ambient{0.25f, 0.25f, 0.25f};
        const ProbeLookup lookup = cascadeLookup(cascades, light, ambient);
        const Vec3 up{0.0f, 1.0f, 0.0f};

        const auto at = [&](const Vec3& point, const ProbeLookup& using_, int from = 0) {
            return sampleProbeCascades(cascades, from, 0.3f, point, up, up, up, up, using_);
        };

        // At the centre: the densest alone.
        const ProbeLights middle = at(centre, lookup);
        assert(nearlyEqual(middle.still.diffuse, light[0], 1e-4f) && nearlyEqual(middle.fast.diffuse, light[0] * 0.5f, 1e-4f));
        assert(nearlyEqual(middle.still.specular, light[0], 1e-4f));

        // Half way through the densest's fade: half each.
        const float halfFade = cascades[0].weightReach.x - 0.5f * cascades[0].weightFade.x;
        const Vec3 between = centre + Vec3{halfFade, 0.0f, 0.0f};
        assert(nearlyEqual(probeCascadeWeight(cascades[0], between), 0.5f, 1e-4f) && probeCascadeWeight(cascades[1], between) == 1.0f);
        assert(nearlyEqual(at(between, lookup).still.diffuse, (light[0] + light[1]) * 0.5f, 1e-3f));

        // Half way through the coarser's fade: half of it, half the flat ambient; fast light fades to nothing.
        const Vec3 outer = centre + Vec3{cascades[1].weightReach.x - 0.5f * cascades[1].weightFade.x, 0.0f, 0.0f};
        const ProbeLights edge = at(outer, lookup);
        assert(nearlyEqual(edge.still.diffuse, (light[1] + ambient) * 0.5f, 1e-3f) && nearlyEqual(edge.fast.diffuse, light[1] * 0.25f, 1e-3f));

        // Past every cascade: the flat ambient.
        const ProbeLights beyond = at(centre + Vec3{100.0f, 0.0f, 0.0f}, lookup);
        assert(nearlyEqual(beyond.still.diffuse, ambient) && nearlyEqual(beyond.fast.diffuse, Vec3{0.0f, 0.0f, 0.0f}));

        // The densest asleep: the coarser lights the centre; from the second cascade on, likewise.
        assert(nearlyEqual(at(centre, cascadeLookup(cascades, light, ambient, 0)).still.diffuse, light[1], 1e-4f));
        assert(nearlyEqual(at(centre, lookup, 1).still.diffuse, light[1], 1e-4f));
    }

    // One fixed volume blends with the flat ambient as the single volume did: mix(ambient, probes, weight).
    void testOneVolumeBlendsAsBefore() {
        const std::vector<ProbeCascade> one{fixedProbeCascade(ProbeVolume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{4, 4, 4}})};
        const std::vector<Vec3> light{Vec3{2.0f, 1.0f, 0.5f}};
        const Vec3 ambient{0.1f, 0.2f, 0.3f};
        const ProbeLookup lookup = cascadeLookup(one, light, ambient);
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const Vec3 point{3.0f + probeVolumeMarginCells + 0.3f * probeVolumeFadeCells, 1.0f, 1.0f};
        const float weight = probeCascadeWeight(one[0], point);
        assert(weight > 0.0f && weight < 1.0f);

        const ProbeLights blended = sampleProbeCascades(one, 0, 0.3f, point, up, up, up, up, lookup);
        assert(nearlyEqual(blended.still.diffuse, ambient * (1.0f - weight) + light[0] * weight, 1e-4f));
        assert(nearlyEqual(blended.fast.diffuse, light[0] * 0.5f * weight, 1e-4f));
    }

    // Packed for the shaders: positions from the camera, the scroll and first probe, the longest distance recorded.
    void testCascadesArePackedForTheShaders() {
        const ProbeCascadeLayout layout{ProbeCounts{5, 3, 5}, 0.5f, 2};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(Vec3{10.3f, 1.2f, -4.4f}, layout, 0.3f);
        const Vec3 camera{10.0f, 1.5f, -4.0f};
        const std::vector<GpuProbeCascade> packed = packProbeCascades(cascades, camera);
        assert(packed.size() == 2);

        for (std::size_t c = 0; c < 2; ++c) {
            const ProbeCascade& cascade = cascades[c];
            const GpuProbeCascade& gpu = packed[c];
            assert(nearlyEqual(Vec3{gpu.origin[0], gpu.origin[1], gpu.origin[2]}, cascade.volume.origin - camera));
            assert(nearlyEqual(gpu.origin[3], probeMaxDistance(cascade.volume.spacing)));
            assert(gpu.spacing[0] == cascade.volume.spacing.x && gpu.spacing[1] == cascade.volume.spacing.y && gpu.spacing[2] == cascade.volume.spacing.z);
            assert(gpu.scroll[0] == cascade.volume.scroll.x && gpu.scroll[1] == cascade.volume.scroll.y && gpu.scroll[2] == cascade.volume.scroll.z);
            assert(gpu.scroll[3] == cascade.firstProbe);
            assert(nearlyEqual(Vec3{gpu.weightCentre[0], gpu.weightCentre[1], gpu.weightCentre[2]}, cascade.weightCentre - camera));
            assert(gpu.weightReach[0] == cascade.weightReach.x && gpu.weightFade[1] == cascade.weightFade.y);
        }
    }

    /*
    * A relocation's records come back a frame or two after they were asked for: each is kept only for a probe still at the
    * lattice point it was asked for at, and a probe a scroll brought in waits, off, for its own.
    */
    void testRecordsWaitForTheirPlace() {
        const ProbeCounts counts{3, 2, 2};
        const std::vector<ProbeCascade> before{probeWindow(Vec3{0.1f, 0.0f, 0.0f}, 1.0f, counts, 0.3f, 0)};
        const std::vector<int> all{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        const std::vector<ProbeRelocationRequest> asked = relocationRequests(before, all);
        assert(asked.size() == 12);

        for (const ProbeRelocationRequest& request : asked) {
            const ProbeGridCoord place = probeLatticeCoord(before[0], request.probe);
            assert(request.place.x == place.x && request.place.y == place.y && request.place.z == place.z);
        }

        // While the pass ran, the window moved a cell along x: the plane it brought in keeps waiting.
        const std::vector<ProbeCascade> after{probeWindow(Vec3{1.1f, 0.0f, 0.0f}, 1.0f, counts, 0.3f, 0)};
        const std::vector<int> scrolled = scrolledProbes(before[0], after[0]);
        assert(scrolled.size() == 4);

        std::vector<GpuProbeRelocation> records(12, pendingProbeRelocation());
        std::vector<GpuProbeRelocation> found(12);

        for (std::size_t i = 0; i < found.size(); ++i) {
            found[i].offset[0] = 0.01f * static_cast<float>(i);
            found[i].surfaceBeyondCells = -1.0f;
        }

        keepProbeRelocations(records, asked, found, after);
        const std::set<int> waiting(scrolled.begin(), scrolled.end());

        for (int probe = 0; probe < 12; ++probe) {
            assert(probeRelocationPending(records[static_cast<std::size_t>(probe)]) == (waiting.count(probe) != 0));
        }

        // Pending is off: never traced or read.
        assert(classifyProbe(pendingProbeRelocation(), 1.0f) == ProbeState::Off);
        assert(!probeRelocationPending(GpuProbeRelocation{}));
    }

    // Each cascade's states from its own records, in order, with its own spacing; no records yet: all vigilant.
    void testEachCascadeHasItsStates() {
        const ProbeCascadeLayout layout{ProbeCounts{3, 3, 3}, 1.0f, 2};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(Vec3{0.0f, 0.0f, 0.0f}, layout, 0.3f);
        const GlobalIlluminationSettings settings;
        const std::vector<GpuProbeState> none = buildCascadeStates(cascades, {}, {}, settings);
        assert(none.size() == 54);
        assert(std::all_of(none.begin(), none.end(), [](const GpuProbeState& state) { return state.state == static_cast<std::int32_t>(ProbeState::Vigilant); }));

        // A record whose nearest surface lies 0.4 m beyond its cells: past the 1 m cascade's bias reach (asleep), within
        // the 2 m one's, which reaches twice as far (vigilant); a pending record: off.
        std::vector<GpuProbeRelocation> records(54);

        for (GpuProbeRelocation& record : records) {
            record.surfaceBeyondCells = 0.4f;
        }

        records[30] = pendingProbeRelocation();
        const std::vector<GpuProbeState> states = buildCascadeStates(cascades, records, {}, settings);
        const float densestReach = probeBiasReach(Vec3{1.0f, 1.0f, 1.0f}, settings.selfShadowBias);
        const float coarserReach = probeBiasReach(Vec3{2.0f, 2.0f, 2.0f}, settings.selfShadowBias);
        assert(densestReach < 0.4f && coarserReach > 0.4f);
        assert(states[0].state == static_cast<std::int32_t>(ProbeState::Sleeping));
        assert(states[27].state == static_cast<std::int32_t>(ProbeState::Vigilant));
        assert(states[30].state == static_cast<std::int32_t>(ProbeState::Off));
    }

    // Probes that start are seeded from the coarser cascades, all but the coarsest's, at most the seeds' room.
    void testSeedsSkipTheCoarsest() {
        const ProbeCascadeLayout layout{ProbeCounts{4, 4, 4}, 1.0f, 3};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(Vec3{0.0f, 0.0f, 0.0f}, layout, 0.3f);
        const std::vector<int> restarted{3, 70, 127, 128, 150, 191};
        const std::vector<ProbeSeed> seeds = chooseProbeSeeds(restarted, cascades);
        assert(seeds.size() == 3);
        assert(seeds[0].probe == 3 && seeds[0].slot == 0 && seeds[1].probe == 70 && seeds[1].slot == 1 && seeds[2].probe == 127 && seeds[2].slot == 2);

        std::vector<int> crowd;

        for (int probe = 0; probe < 128; ++probe) {
            for (int repeat = 0; repeat < 20; ++repeat) {
                crowd.push_back(probe);
            }
        }

        assert(chooseProbeSeeds(crowd, cascades).size() == static_cast<std::size_t>(maxProbeSeeds));
    }

    /*
    * A seeded probe's restart starts as if its seed were probeSeedFrames frames of its own: the share kept on its first
    * frame is that of a restart that many frames old, its seed slot given for that frame only, and its frames count on
    * from there; distances restart outright. Unseeded ones restart from nothing, as before.
    */
    void testSeededRestartsStartFromTheirSeed() {
        const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{4, 1, 1}};
        ProbeFrame frame;
        frame.irradianceHysteresis = 0.98f;
        frame.fastIrradianceHysteresis = 0.8f;
        frame.distanceHysteresis = 0.96f;
        frame.restartHysteresis = 0.5f;
        frame.restartGrowth = 0.5f;
        frame.restartCheckFrames = 30.0f;

        std::vector<GpuProbeRestart> restarts;
        const std::vector<int> restarted{1, 2};
        const std::vector<ProbeSeed> seeded{ProbeSeed{2, 5}};
        advanceProbeRestarts(restarts, volume, {}, frame, restarted, seeded);

        assert(restarts[1].irradianceFrames == 0 && restarts[1].irradianceKeep == 0.0f && restarts[1].seed == -1);
        assert(restarts[2].irradianceFrames == static_cast<std::int32_t>(probeSeedFrames) && restarts[2].seed == 5);
        assert(nearlyEqual(restarts[2].irradianceKeep, restartedProbeKeep(0.98f, 0.5f, 0.5f, probeSeedFrames)));
        assert(nearlyEqual(restarts[2].fastKeep, restartedProbeKeep(0.8f, 0.0f, 0.5f, probeSeedFrames)));
        assert(restarts[2].distanceFrames == 0 && restarts[2].distanceKeep == 0.0f);
        assert(restarts[0].seed == -1 && restarts[3].seed == -1);

        advanceProbeRestarts(restarts, volume, {}, frame);
        assert(restarts[2].seed == -1 && restarts[2].irradianceFrames == static_cast<std::int32_t>(probeSeedFrames) + 1);
        assert(restarts[1].irradianceFrames == 1);

        const AdaptiveProbeTexel seed = seededProbeTexel(Vec3{0.3f, 0.2f, 0.1f});
        assert(seed.stored.x == 0.3f && seed.recent.y == 0.2f && seed.history == -probeSeedFrames && seed.noise == 0.0f);
    }

    // Each cascade's events against its own probes: a region round a point restarts the probes inside it in every cascade.
    void testEventsReachEveryCascade() {
        const ProbeCascadeLayout layout{ProbeCounts{5, 5, 5}, 1.0f, 2};
        const std::vector<ProbeCascade> cascades = probeCascadesAround(Vec3{0.0f, 0.0f, 0.0f}, layout, 0.3f);
        const std::vector<ProbeVolume> volumes{cascades[0].volume, cascades[1].volume};
        ProbeFrame frame;
        frame.irradianceHysteresis = 0.98f;
        frame.distanceHysteresis = 0.96f;
        const ProbeEvent event = probeEventAround(ProbeEventKind::LightChange, Vec3{0.0f, 0.0f, 0.0f}, 1.5f);
        std::vector<GpuProbeRestart> restarts;
        advanceProbeRestarts(restarts, volumes, std::span<const ProbeEvent>{&event, 1}, frame);
        assert(restarts.size() == 250);

        for (std::size_t c = 0; c < 2; ++c) {
            for (int slot = 0; slot < 125; ++slot) {
                const bool inside = event.region.contains(probePosition(volumes[c], slot));
                assert((restarts[c * 125 + static_cast<std::size_t>(slot)].irradianceFrames == 0) == inside);
            }
        }

        // 27 probes 1 m apart within 1.5 m, 1 at 2 m apart.
        const long restartedCount = std::count_if(restarts.begin(), restarts.end(), [](const GpuProbeRestart& restart) { return restart.irradianceFrames == 0; });
        assert(restartedCount == 28);
    }

    /*
    * What the seed is for, in a dark room's texel (a small bright patch, as ddgi_test's model, 128 rays at 60 fps): a
    * probe a scroll brings in, read soon after. Its first frames from its own rays alone are few and noisy; a seed from
    * the coarser cascade, a little off, makes its first quarter second calmer and still lets its own light take over.
    * Returns the RMS error over frames [from, to) as a share of the true light, over many starts. Measured: the first
    * quarter second 13.3% from its rays alone, 9.6% seeded 20% off, 23.8% seeded twice the light, 24.3% seeded with
    * none; from half a second to a second 2.4%, 2.8% and 2.7% (GlobalIlluminationSettings::seedProbes).
    */
    double startedTexelError(bool seeded, float seedShare, int from, int to) {
        const Vec3 towardsPatch{1.0f, 0.0f, 0.0f};
        const float patchCosine = std::cos(radiansFromDegrees(20.0f));
        const Vec3 texelDirection = probeTexelDirection(TexelCoord{4, 4}, probeIrradianceTexels);
        const int rays = 128;
        const float frameSeconds = 1.0f / 60.0f;
        const float longKeep = hysteresisForTimeConstant(frameSeconds, 2.0f);
        const float windowKeep = hysteresisForTimeConstant(frameSeconds, 0.2f);

        const auto estimateAt = [&](std::uint64_t frameNumber) {
            const ProbeRayFrame rayFrame = probeRayFrame(frameNumber);
            const ProbeRotationRows rows = probeRotationRows(rayFrame.rotation);
            std::vector<Vec3> directions;
            std::vector<Vec3> radiance;

            for (int ray = 0; ray < rays; ++ray) {
                const Vec3 direction = probeRayDirection(ray, rays, rayFrame.phase, rows);
                const float light = direction.dot(towardsPatch) > patchCosine ? 3.0f : 0.01f;
                directions.push_back(direction);
                radiance.push_back(Vec3{light, light, light});
            }

            return irradianceEstimate(texelDirection, directions, radiance).x;
        };

        double truth = 0.0;

        for (std::uint64_t frameNumber = 0; frameNumber < 3200; ++frameNumber) {
            truth += estimateAt(frameNumber + (1u << 22)) / 3200.0;
        }

        ProbeFrame restartFrame;
        restartFrame.restartGrowth = 0.5f;
        restartFrame.restartCheckFrames = eventCheckFrames(0.5f, frameSeconds);
        const ProbeEventCheck check{0.0f, 0.5f, restartFrame.restartCheckFrames};
        double squares = 0.0;
        long counted = 0;

        for (int start = 0; start < 64; ++start) {
            const float seed = static_cast<float>(truth) * seedShare;
            AdaptiveProbeTexel texel = seeded ? seededProbeTexel(Vec3{seed, seed, seed}) : AdaptiveProbeTexel{};
            int since = seeded ? static_cast<int>(probeSeedFrames) : 0;

            for (int i = 0; i < to; ++i) {
                const float estimate = estimateAt(static_cast<std::uint64_t>(start) * 997u + static_cast<std::uint64_t>(i));
                texel = adaptiveProbeUpdate(texel, Vec3{estimate, estimate, estimate}, longKeep, windowKeep, 8.0f, since, check);
                since = advanceRestartFrames(since, longKeep, restartFrame);

                if (i >= from) {
                    const double error = (texel.stored.x - truth) / truth;
                    squares += error * error;
                    ++counted;
                }
            }
        }

        return std::sqrt(squares / static_cast<double>(counted));
    }

    void testASeedSettlesAStartedProbeSooner() {
        const int quarter = 15;
        const double unseeded = startedTexelError(false, 1.0f, 0, quarter);
        const double closeSeed = startedTexelError(true, 1.2f, 0, quarter);
        assert(closeSeed < 0.8 * unseeded);

        // A seed far off (twice the light, or none) fades: half a second on it errs little more than no seed does.
        const double settledUnseeded = startedTexelError(false, 1.0f, 30, 60);
        assert(startedTexelError(true, 2.0f, 30, 60) < settledUnseeded + 0.03);
        assert(startedTexelError(true, 0.0f, 30, 60) < settledUnseeded + 0.03);
    }
}

void testProbeCascades() {
    testTheLayoutFollowsTheSettings();
    testSlotsWrapRound();
    testAWindowIsCentredOnTheLattice();
    testAScrollBringsInOnePlane();
    testCascadesDoubleAndNest();
    testTheWeightNeverReadsAnOuterPlane();
    testTheWeightFadesRoundTheCentre();
    testAFixedVolumeFadesBeyondItsProbes();
    testCascadesBlendDensestFirst();
    testOneVolumeBlendsAsBefore();
    testCascadesArePackedForTheShaders();
    testRecordsWaitForTheirPlace();
    testEachCascadeHasItsStates();
    testSeedsSkipTheCoarsest();
    testSeededRestartsStartFromTheirSeed();
    testEventsReachEveryCascade();
    testASeedSettlesAStartedProbeSooner();
}
