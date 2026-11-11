#include "core/job_system.h"
#include "engine/settings_file.h"
#include "render/gpu/probe_cascades.h"
#include "render/gpu/quality_preset.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/sky_visibility.h"
#include "render/gpu/sky_visibility_bake.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    constexpr int manyRays = 256;

    bool rejects(const SkyVisibilitySettings& settings) {
        try {
            validateSkyVisibility(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // The bands of a sphere open wherever open(direction) says, from many even rays.
    template <typename Open>
    SkyVisibility bandsOf(Open open) {
        std::vector<Vec3> directions;
        std::vector<float> openings;

        for (int ray = 0; ray < manyRays; ++ray) {
            directions.push_back(skyVisibilityRay(ray, manyRays));
            openings.push_back(open(directions.back()) ? 1.0f : 0.0f);
        }

        return skyVisibilityFromRays(directions, openings);
    }

    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 down{0.0f, -1.0f, 0.0f};
    const Vec3 east{1.0f, 0.0f, 0.0f};

    bool nearColour(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    Vec3 grey(float value) {
        return Vec3{value, value, value};
    }

    // Each colour channel's bands of a light coming from wherever open(direction) says, times that channel's value.
    template <typename Open>
    SkyBounce bounceOf(Open open, const Vec3& colour) {
        const SkyVisibility bands = bandsOf(open);
        SkyBounce bounce = noSkyBounce;
        const float channels[3] = {colour.x, colour.y, colour.z};

        for (std::size_t channel = 0; channel < 3; ++channel) {
            bounce[channel] = SkyVisibility{bands.open * channels[channel], bands.lean * channels[channel]};
        }

        return bounce;
    }

    /*
    * An open field: the sky above, the ground below. The bands are a = 1/2 and b = 3/4 up, so a floor gets all of the
    * sky's light (as the flat ambient assumed for every surface), a wall half and a ceiling none of the sky; with the
    * ground's bounce (albedo rho, lit by the whole sky), a ceiling gets rho, a wall half of 1 and half of rho.
    */
    void testAnOpenFieldLightsTheFloorFully() {
        const SkyVisibility field = bandsOf([](const Vec3& direction) { return direction.y > 0.0f; });
        assert(near(field.open, 0.5f, 0.01f));
        assert(near(field.lean.y, 0.75f, 0.02f) && near(field.lean.x, 0.0f, 0.02f) && near(field.lean.z, 0.0f, 0.02f));

        assert(nearColour(skyLightFacing(field, noSkyBounce, up, 0.0f), grey(1.0f), 0.02f));
        assert(nearColour(skyLightFacing(field, noSkyBounce, east, 0.0f), grey(0.5f), 0.02f));
        assert(nearColour(skyLightFacing(field, noSkyBounce, down, 0.0f), grey(0.0f), 0.02f));
        assert(nearColour(skyLightInAir(field, noSkyBounce, 0.0f), grey(0.5f), 0.01f));
        assert(nearColour(skyLightAlong(field, noSkyBounce, up, 0.0f), grey(1.0f), 0.02f));

        const Vec3 rho{0.8f, 0.4f, 0.2f};
        const SkyBounce ground = bounceOf([](const Vec3& direction) { return direction.y < 0.0f; }, rho);
        assert(nearColour(skyLightFacing(field, ground, down, 0.0f), rho, 0.02f));
        assert(nearColour(skyLightFacing(field, ground, up, 0.0f), grey(1.0f), 0.02f));
        assert(nearColour(skyLightFacing(field, ground, east, 0.0f), grey(0.5f) + rho * 0.5f, 0.02f));
        assert(nearColour(skyLightInAir(field, ground, 0.0f), grey(0.5f) + rho * 0.5f, 0.01f));
        // Along one direction two bands overshoot a hemisphere by a quarter (a + b . d = 1.25), each channel kept within 0..1.
        assert(nearColour(skyLightAlong(field, ground, down, 0.0f), Vec3{1.0f, 0.5f, 0.25f}, 0.03f));
    }

    // A closed box sees no sky, and nothing lit bounces into it: dark, or the floor (dim, not black); with everything
    // open, every surface gets all.
    void testAClosedBoxIsDark() {
        const SkyVisibility box = bandsOf([](const Vec3&) { return false; });
        const SkyVisibility open = bandsOf([](const Vec3&) { return true; });

        for (const Vec3& normal : {up, down, east}) {
            assert(skyLightFacing(box, noSkyBounce, normal, 0.0f).x == 0.0f);
            assert(nearColour(skyLightFacing(box, noSkyBounce, normal, 0.025f), grey(0.025f), 1e-6f));
            assert(nearColour(skyLightFacing(open, noSkyBounce, normal, 0.025f), grey(1.0f), 1e-4f));
        }

        assert(nearColour(skyLightInAir(box, noSkyBounce, 0.03f), grey(0.03f), 1e-6f));
        assert(nearColour(skyLightAlong(box, noSkyBounce, up, 0.03f), grey(0.03f), 1e-6f));
    }

    // Two bands blur sharp edges and can overshoot: the sky seen and each bounce channel are kept within 0..1 before
    // they are added.
    void testTheShareStaysWithinItsRange() {
        const SkyVisibility ringing{0.2f, Vec3{0.0f, 0.9f, 0.0f}};
        assert(nearColour(skyLightFacing(ringing, noSkyBounce, down, 0.0f), grey(0.0f), 1e-6f));
        assert(nearColour(skyLightAlong(ringing, noSkyBounce, up, 0.0f), grey(1.0f), 1e-6f));
        assert(nearColour(skyLightFacing(ringing, noSkyBounce, up, 0.0f), grey(0.8f), 1e-5f));

        SkyBounce overshoot = noSkyBounce;
        overshoot[0] = SkyVisibility{0.3f, Vec3{0.0f, -0.9f, 0.0f}};
        const Vec3 facingDown = skyLightFacing(ringing, overshoot, down, 0.0f);
        assert(near(facingDown.x, 0.9f, 1e-5f) && facingDown.y == 0.0f);
        assert(skyLightFacing(ringing, overshoot, up, 0.0f).x == skyLightFacing(ringing, noSkyBounce, up, 0.0f).x);
    }

    // A point whose open side leans up sees the sky facing up, and none looking straight down.
    void testSeenFacingAndAlong() {
        assert(near(skySeenFacing(SkyVisibility{0.2f, Vec3{0.0f, 0.9f, 0.0f}}, up), 0.8f, 1e-6f));
        assert(skySeenAlong(SkyVisibility{0.2f, Vec3{0.0f, 0.9f, 0.0f}}, down) == 0.0f);
    }

    // A grid point sees what lies within its reach along each axis: a wall a third of a spacing east hides the far side.
    void testACornerSeesWithinItsReach() {
        std::array<float, skyReachDirections> reach{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        const float spacing = 0.5f;
        assert(skyCornerSees(reach, Vec3{0.5f, -0.5f, 0.49f}, spacing));

        reach[0] = 1.0f / 3.0f;
        assert(!skyCornerSees(reach, Vec3{0.25f, 0.0f, 0.0f}, spacing));
        assert(skyCornerSees(reach, Vec3{0.16f, 0.1f, -0.4f}, spacing));
        assert(skyCornerSees(reach, Vec3{-0.4f, 0.0f, 0.0f}, spacing));
        assert(skyCornerSees(reach, Vec3{spacing / 3.0f, 0.0f, 0.0f}, spacing));

        for (int direction = 0; direction < skyReachDirections; ++direction) {
            std::array<float, skyReachDirections> blocked{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            blocked[static_cast<std::size_t>(direction)] = 0.0f;
            assert(!skyCornerSees(blocked, skyReachDirection(direction) * 0.2f, spacing));
            assert(skyCornerSees(blocked, skyReachDirection(direction) * -0.2f, spacing));
        }
    }

    // The packed point keeps its bands to a half's precision, its reaches to a 255th and its state exactly.
    void testAPointPacksForTheShaders() {
        SkyCell cell;
        cell.visibility = SkyVisibility{0.375f, Vec3{-0.25f, 0.5f, 0.125f}};
        cell.reach = {0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 0.1f};
        cell.state = SkyCellState::Baked;

        cell.bounce = {SkyVisibility{0.25f, Vec3{0.0625f, -0.125f, 0.5f}}, SkyVisibility{0.5f, Vec3{0.25f, 0.0f, -0.75f}},
                       SkyVisibility{0.125f, Vec3{-0.5f, 0.375f, 0.0f}}};

        const SkyCell back = unpackSkyCell(packSkyCell(cell));
        assert(back.visibility.open == 0.375f && back.visibility.lean.x == -0.25f && back.visibility.lean.y == 0.5f && back.visibility.lean.z == 0.125f);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            assert(back.bounce[channel].open == cell.bounce[channel].open);
            assert(back.bounce[channel].lean.x == cell.bounce[channel].lean.x && back.bounce[channel].lean.y == cell.bounce[channel].lean.y &&
                   back.bounce[channel].lean.z == cell.bounce[channel].lean.z);
        }
        for (std::size_t direction = 0; direction < cell.reach.size(); ++direction) {
            assert(near(back.reach[direction], cell.reach[direction], 0.5f / skyVisibilityReachSteps + 1e-6f));
        }
        assert(back.state == SkyCellState::Baked);

        // A reach past a whole spacing is a whole spacing.
        cell.reach[0] = 7.0f;
        cell.state = SkyCellState::Buried;
        const SkyCell buried = unpackSkyCell(packSkyCell(cell));
        assert(buried.reach[0] == 1.0f && buried.state == SkyCellState::Buried);
        assert(unpackSkyCell(packSkyCell(SkyCell{})).state == SkyCellState::Unknown);
        assert(unpackSkyCell(packSkyCell(SkyCell{})).bounce[1].open == 0.0f);
    }

    // The cascades: each the same window of points, twice the last one's spacing, numbered one after another.
    void testTheCascadesNestRoundTheCamera() {
        SkyVisibilitySettings settings;
        settings.cascades = 3;
        const Vec3 centre{10.3f, 1.7f, -4.2f};
        const std::vector<ProbeCascade> cascades = skyVisibilityCascades(centre, settings);
        const int perCascade = SKY_VISIBILITY_ACROSS * SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS;
        assert(cascades.size() == 3);
        assert(skyVisibilityCellCount(settings) == 3 * perCascade);

        for (int c = 0; c < 3; ++c) {
            const ProbeCascade& cascade = cascades[static_cast<std::size_t>(c)];
            assert(cascade.firstProbe == c * perCascade);
            assert(cascade.volume.counts.x == SKY_VISIBILITY_ACROSS && cascade.volume.counts.y == SKY_VISIBILITY_HIGH);
            assert(cascade.volume.spacing.x == skyVisibilitySpacing(settings, c));
            assert(probeCascadeWeight(cascade, centre) == 1.0f);
        }
        assert(skyVisibilitySpacing(settings, 2) == 4.0f * settings.spacing);

        // No point on a whole or half metre, where floors are put.
        const float y = cascades[0].volume.origin.y;
        assert(std::abs(y * 2.0f - std::round(y * 2.0f)) > 0.01f);
    }

    // One cascade's points with every one set by place(lattice point's world position) and the cascade itself.
    template <typename Place>
    std::vector<SkyCell> placedCells(const std::vector<ProbeCascade>& cascades, int count, Place place) {
        std::vector<SkyCell> cells(static_cast<std::size_t>(count));

        for (const ProbeCascade& cascade : cascades) {
            const int points = probeCount(cascade.volume.counts);
            for (int slot = 0; slot < points; ++slot) {
                cells[static_cast<std::size_t>(cascade.firstProbe + slot)] = place(probePosition(cascade.volume, slot));
            }
        }

        return cells;
    }

    SkyCell bakedCell(const SkyVisibility& visibility, const SkyBounce& bounce = noSkyBounce) {
        SkyCell cell;
        cell.visibility = visibility;
        cell.bounce = bounce;
        cell.state = SkyCellState::Baked;
        return cell;
    }

    /*
    * Nothing baked yet reads as an open field (the sky above, the ground below, a = 1/2 and b = 3/4 up), with no bounce
    * and nothing known: a floor gets the whole flat ambient, as before the grid, but a wall or the air only half, as
    * inside the grid in the open, so the fog and walls past the grid do not jump to twice the light. Points that all
    * agree read as that, bounce too, wholly known.
    */
    void testReadingTheGrid() {
        SkyVisibilitySettings settings;
        const Vec3 centre{0.37f, 1.61f, -0.22f};
        const std::vector<ProbeCascade> cascades = skyVisibilityCascades(centre, settings);
        const int count = skyVisibilityCellCount(settings);

        const std::vector<SkyCell> unknown(static_cast<std::size_t>(count));
        const SkySample open = sampleSkyVisibility(cascades, unknown, settings.lookupBias, centre, up, up);
        assert(open.visibility.open == skyOpenField.open && open.visibility.lean.x == 0.0f && open.visibility.lean.y == skyOpenField.lean.y &&
               open.visibility.lean.z == 0.0f);
        assert(near(skyOpenField.open, 0.5f, 0.0f) && near(skyOpenField.lean.y, 0.75f, 0.0f));
        assert(nearColour(skyLightFacing(open.visibility, open.bounce, up, 0.0f), grey(1.0f), 1e-5f));
        assert(nearColour(skyLightFacing(open.visibility, open.bounce, east, 0.0f), grey(0.5f), 1e-5f));
        assert(nearColour(skyLightInAir(open.visibility, open.bounce, 0.0f), grey(0.5f), 1e-5f));
        assert(open.known == 0.0f && open.bounce[0].open == 0.0f);
        assert(open.missing == 1.0f); // inside the grid, nothing baked there yet

        const SkyVisibility room{0.1f, Vec3{0.05f, 0.02f, 0.0f}};
        const SkyBounce walls{SkyVisibility{0.3f, Vec3{0.1f, 0.0f, 0.0f}}, SkyVisibility{0.2f, Vec3{}}, SkyVisibility{0.1f, Vec3{0.0f, 0.0f, -0.05f}}};
        const std::vector<SkyCell> same = placedCells(cascades, count, [&](const Vec3&) { return bakedCell(room, walls); });
        const SkySample read = sampleSkyVisibility(cascades, same, settings.lookupBias, centre, up, up);
        assert(near(read.visibility.open, room.open, 1e-5f) && near(read.visibility.lean.x, room.lean.x, 1e-5f) && near(read.visibility.lean.y, room.lean.y, 1e-5f));
        assert(near(read.known, 1.0f, 1e-5f) && read.missing == 0.0f);
        assert(near(read.bounce[0].open, 0.3f, 1e-5f) && near(read.bounce[0].lean.x, 0.1f, 1e-5f) && near(read.bounce[2].lean.z, -0.05f, 1e-5f));

        // Far past the last cascade, the open field again, nothing bounced and nothing known.
        const SkySample far = sampleSkyVisibility(cascades, same, settings.lookupBias, centre + Vec3{500.0f, 0.0f, 0.0f}, up, up);
        assert(far.visibility.open == skyOpenField.open && far.visibility.lean.y == skyOpenField.lean.y && far.known == 0.0f && far.bounce[1].open == 0.0f);
        assert(far.missing == 0.0f); // past the grid: not missing, just not there

        // The air where the grid reaches but has no point to read (inside a wall, every point round it buried) stays
        // dark, at the floor: fog samples jittered across a wall would otherwise flip between a room's light and the
        // open field's. Past the grid nothing is missing, so it keeps the open field; a known point is unchanged.
        std::vector<SkyCell> buried(static_cast<std::size_t>(count));
        for (SkyCell& cell : buried) {
            cell.state = SkyCellState::Buried;
        }
        const SkySample walled = skyMissingLeftDark(sampleSkyVisibility(cascades, buried, settings.lookupBias, centre, Vec3{}, Vec3{}));
        assert(nearColour(skyLightInAir(walled.visibility, walled.bounce, 0.025f), grey(0.025f), 1e-6f));
        const SkySample farAir = skyMissingLeftDark(far);
        assert(farAir.visibility.open == skyOpenField.open && farAir.visibility.lean.y == skyOpenField.lean.y);
        const SkySample readAir = skyMissingLeftDark(read);
        assert(readAir.visibility.open == read.visibility.open && readAir.visibility.lean.x == read.visibility.lean.x);
    }

    /*
    * Leaks: points west of the surface point are dark, east of it open. Left out as buried, or seeing it through no
    * wall (their reach east cut to nothing), they add nothing, and the point reads the open side alone.
    */
    void testPointsInWallsOrBehindThemAreLeftOut() {
        SkyVisibilitySettings settings;
        settings.cascades = 1;
        const Vec3 centre{0.52f, 1.61f, -0.22f};
        const std::vector<ProbeCascade> cascades = skyVisibilityCascades(centre, settings);
        const int count = skyVisibilityCellCount(settings);
        const SkyVisibility dark{0.0f, Vec3{0.0f, 0.0f, 0.0f}};
        const SkyVisibility sky{1.0f, Vec3{0.0f, 0.0f, 0.0f}};

        const std::vector<SkyCell> buried = placedCells(cascades, count, [&](const Vec3& position) {
            SkyCell cell = bakedCell(position.x < centre.x ? dark : sky);
            cell.state = position.x < centre.x ? SkyCellState::Buried : SkyCellState::Baked;
            return cell;
        });
        assert(near(sampleSkyVisibility(cascades, buried, settings.lookupBias, centre, up, up).visibility.open, 1.0f, 1e-5f));

        const std::vector<SkyCell> walled = placedCells(cascades, count, [&](const Vec3& position) {
            SkyCell cell = bakedCell(position.x < centre.x ? dark : sky);
            if (position.x < centre.x) {
                cell.reach[0] = 0.0f;
            }
            return cell;
        });
        assert(near(sampleSkyVisibility(cascades, walled, settings.lookupBias, centre, up, up).visibility.open, 1.0f, 1e-5f));

        // Without the wall both sides count.
        const std::vector<SkyCell> both = placedCells(cascades, count, [&](const Vec3& position) { return bakedCell(position.x < centre.x ? dark : sky); });
        const float mixed = sampleSkyVisibility(cascades, both, settings.lookupBias, centre, up, up).visibility.open;
        assert(mixed > 0.2f && mixed < 0.8f);
    }

    // A floor point reads the points above it far more than those below, behind its surface (its lookup lifted near the
    // plane above, as a real floor's is; the points below a real floor are inside it, buried).
    void testAFloorReadsWhatIsAboveIt() {
        SkyVisibilitySettings settings;
        settings.cascades = 1;
        const Vec3 floorPoint{0.37f, 2.1f, -0.22f};
        const std::vector<ProbeCascade> cascades = skyVisibilityCascades(floorPoint, settings);
        const std::vector<SkyCell> cells = placedCells(cascades, skyVisibilityCellCount(settings), [&](const Vec3& position) {
            return bakedCell(SkyVisibility{position.y > floorPoint.y ? 1.0f : 0.0f, Vec3{0.0f, 0.0f, 0.0f}});
        });

        assert(sampleSkyVisibility(cascades, cells, settings.lookupBias, floorPoint, up, up).visibility.open > 0.75f);
        assert(sampleSkyVisibility(cascades, cells, settings.lookupBias, floorPoint, down, down).visibility.open < 0.3f);
    }

    void testTheSettings() {
        const SkyVisibilitySettings defaults;
        assert(defaults.enabled && defaults.spacing == 0.5f && defaults.cascades == 2);
        assert(defaults.rays == 256 && defaults.rayLength == 30.0f && defaults.bakeMilliseconds == 1.0f && !defaults.debugView);
        assert(defaults.ambientFloor == 0.025f && defaults.quickRays == 64 && defaults.storeMegabytes == 256.0f);
        validateSkyVisibility(defaults);

        SkyVisibilitySettings bad;
        bad.ambientFloor = 1.5f;
        assert(rejects(bad));
        bad.ambientFloor = -0.1f;
        assert(rejects(bad));
        bad = {};
        bad.spacing = 0.0f;
        assert(rejects(bad));
        bad = {};
        bad.cascades = maxSkyVisibilityCascades + 1;
        assert(rejects(bad));
        bad = {};
        bad.rays = 0;
        assert(rejects(bad));
        bad = {};
        bad.quickRays = 0;
        assert(rejects(bad));
        bad = {};
        bad.storeMegabytes = -1.0f;
        assert(rejects(bad));
        bad.storeMegabytes = 0.0f;
        assert(!rejects(bad));
        bad = {};
        bad.rayLength = -1.0f;
        assert(rejects(bad));
        bad = {};
        bad.bakeMilliseconds = -1.0f;
        assert(rejects(bad));
        bad = {};
        bad.bakeMilliseconds = 0.0f;
        assert(!rejects(bad));

        // Only what shapes the grid bakes it again.
        SkyVisibilitySettings other;
        other.lookupBias = 0.1f;
        other.bakeMilliseconds = 3.0f;
        other.debugView = true;
        other.ambientFloor = 0.3f;
        other.quickRays = 16;         // only how a refill gets there, not what it ends as
        other.storeMegabytes = 32.0f; // only what is kept, not what is baked
        assert(sameSkyVisibilityGrid(defaults, other));
        other.rays = 32;
        assert(!sameSkyVisibilityGrid(defaults, other));

        // Reachable by key from the command line and the settings file.
        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings =
            applySettings({{"skyVisibility.enabled", "false"}, {"skyVisibility.debugView", "true"}, {"skyVisibility.ambientFloor", "0.2"}}, walk, StartupFields::Keep);
        assert(warnings.empty() && !settings.skyVisibility.enabled && settings.skyVisibility.debugView);
        assert(settings.skyVisibility.ambientFloor == 0.2f);

        // The shaders' options: nothing while not read; x the cascades read, y the floor, z the lookup bias, w the view.
        const GpuSkyVisibilityOptions off = packSkyVisibilityOptions(defaults, 0);
        assert(off.values[0] == 0.0f && off.values[1] == 0.0f && off.values[2] == 0.0f && off.values[3] == 0.0f);
        const GpuSkyVisibilityOptions on = packSkyVisibilityOptions(other, 2);
        assert(on.values[0] == 2.0f && on.values[1] == 0.3f && on.values[2] == 0.1f && on.values[3] == 1.0f);
    }

    // Off on Low, as ambient occlusion is; on from Medium up.
    void testThePresets() {
        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            RenderSettings settings;
            settings.skyVisibility.enabled = preset == QualityPreset::Low;
            applyQualityPreset(settings, preset);
            assert(settings.skyVisibility.enabled == (preset != QualityPreset::Low));
            assert(matchingQualityPreset(settings) == preset);
        }
    }

    // Above the open floor: half the sphere open, all of the sky on a floor; the floor a fifth of a spacing below.
    void testBakingAPointOverAFloor() {
        const StaticLevel level = flatLevel();
        GridMarks marks;
        const SkyCell cell = bakeSkyCell(level, Vec3{0.3f, 0.1f, 0.2f}, manyRays, 30.0f, 0.5f, marks);
        assert(cell.state == SkyCellState::Baked);
        assert(near(skySeenFacing(cell.visibility, up), 1.0f, 0.03f));
        assert(near(skySeenFacing(cell.visibility, down), 0.0f, 0.03f));
        assert(near(cell.reach[3], 0.2f, 0.01f));
        assert(cell.reach[0] == 1.0f && cell.reach[1] == 1.0f && cell.reach[2] == 1.0f && cell.reach[4] == 1.0f && cell.reach[5] == 1.0f);

        // A ray running past its length counts as open: a roof 40 m up does not shade a point given 30 m.
        StaticLevel roofed = flatLevel();
        roofed.add(block(Vec3{-50.0f, 40.0f, -50.0f}, Vec3{50.0f, 41.0f, 50.0f}));
        const SkyCell under = bakeSkyCell(roofed, Vec3{0.3f, 1.0f, 0.2f}, manyRays, 30.0f, 0.5f, marks);
        assert(near(skySeenFacing(under.visibility, up), 1.0f, 0.03f));
        const SkyCell roofSeen = bakeSkyCell(roofed, Vec3{0.3f, 1.0f, 0.2f}, manyRays, 100.0f, 0.5f, marks);
        assert(skySeenFacing(roofSeen.visibility, up) < 0.6f);
    }

    // A room of walls 0.2 m thick, 6 by 3 by 6 m inside, with a doorway 1 m wide and 2 m high in its east wall.
    StaticLevel roomWithADoor() {
        StaticLevel level = flatLevel();
        level.add(block(Vec3{-3.2f, 0.0f, -3.2f}, Vec3{3.2f, 3.0f, -3.0f}));  // north
        level.add(block(Vec3{-3.2f, 0.0f, 3.0f}, Vec3{3.2f, 3.0f, 3.2f}));    // south
        level.add(block(Vec3{-3.2f, 0.0f, -3.0f}, Vec3{-3.0f, 3.0f, 3.0f}));  // west
        level.add(block(Vec3{3.0f, 0.0f, -3.0f}, Vec3{3.2f, 3.0f, -0.5f}));   // east, north of the door
        level.add(block(Vec3{3.0f, 0.0f, 0.5f}, Vec3{3.2f, 3.0f, 3.0f}));     // east, south of the door
        level.add(block(Vec3{3.0f, 2.0f, -0.5f}, Vec3{3.2f, 3.0f, 0.5f}));    // over the door
        level.add(block(Vec3{-3.2f, 3.0f, -3.2f}, Vec3{3.2f, 3.2f, 3.2f}));   // roof
        return level;
    }

    // Inside, little sky, more facing the door than away; inside a wall, buried.
    void testBakingARoom() {
        const StaticLevel level = roomWithADoor();
        GridMarks marks;
        const SkyCell inside = bakeSkyCell(level, Vec3{1.5f, 1.0f, 0.1f}, manyRays, 30.0f, 0.5f, marks);
        assert(inside.state == SkyCellState::Baked);
        const float towardsDoor = skySeenFacing(inside.visibility, east);
        const float awayFromDoor = skySeenFacing(inside.visibility, Vec3{-1.0f, 0.0f, 0.0f});
        assert(inside.visibility.open < 0.1f);
        assert(towardsDoor > awayFromDoor);
        assert(skySeenFacing(inside.visibility, up) < 0.15f);

        const SkyCell inWall = bakeSkyCell(level, Vec3{-3.1f, 1.0f, 0.1f}, manyRays, 30.0f, 0.5f, marks);
        assert(inWall.state == SkyCellState::Buried);

        // Beside the west wall's inside face, 0.1 m off: its reach west is a fifth of a spacing.
        const SkyCell byWall = bakeSkyCell(level, Vec3{-2.9f, 1.0f, 0.1f}, 8, 30.0f, 0.5f, marks);
        assert(near(byWall.reach[1], 0.2f, 0.01f) && byWall.reach[0] == 1.0f);
    }

    std::vector<Vec3> albedoOf(const StaticLevel& level, const Vec3& colour) {
        return std::vector<Vec3>(level.getTriangleCount(), colour);
    }

    // What the grid says round a hit: the whole sphere open (a point under an open sky), wholly known.
    SkySample openSkyAt(const Vec3&, const Vec3&, const Vec3&) {
        SkySample sample;
        sample.visibility = SkyVisibility{1.0f, Vec3{}};
        sample.known = 1.0f;
        return sample;
    }

    // Nothing known round a hit yet.
    SkySample unknownAt(const Vec3&, const Vec3&, const Vec3&) {
        return SkySample{};
    }

    /*
    * The light on a hit, per unit of the open sky's light: the sky it sees facing its normal plus the bounce there; where
    * the grid knows less than half of what is round it, the fallback grey instead.
    */
    void testTheLightOnAHit() {
        SkySample field;
        field.visibility = bandsOf([](const Vec3& direction) { return direction.y > 0.0f; });
        field.bounce = bounceOf([](const Vec3& direction) { return direction.y < 0.0f; }, Vec3{0.8f, 0.4f, 0.2f});
        field.known = 1.0f;
        assert(nearColour(skyHitLight(field, up, 0.3f), grey(1.0f), 0.02f));
        assert(nearColour(skyHitLight(field, down, 0.3f), Vec3{0.8f, 0.4f, 0.2f}, 0.02f));

        field.known = 0.4f;
        assert(nearColour(skyHitLight(field, up, 0.3f), grey(0.3f), 1e-6f));
    }

    /*
    * The bounce over an open floor of albedo rho, its points lit by the whole sky (the analytic case: facing down the
    * hemisphere is all floor, of radiance rho L under a sky of L, so rho of the sky's light; facing up none; sideways
    * half). The same with nothing known round the hits, through the guess (twice the point's open share, here all).
    * Without albedo or a way to read the grid, no bounce.
    */
    void testBounceOverAFloor() {
        const StaticLevel level = flatLevel();
        const Vec3 rho{0.8f, 0.4f, 0.2f};
        const std::vector<Vec3> albedo = albedoOf(level, rho);
        const Vec3 point{0.3f, 0.1f, 0.2f};
        GridMarks marks;

        const SkyCell cell = bakeSkyCell(level, point, manyRays, 30.0f, 0.5f, marks, albedo, openSkyAt);
        assert(nearColour(skyLightFacing(cell.visibility, cell.bounce, down, 0.0f), rho, 0.03f));
        assert(nearColour(skyLightFacing(cell.visibility, cell.bounce, up, 0.0f), grey(1.0f), 0.03f));
        assert(nearColour(skyLightFacing(cell.visibility, cell.bounce, east, 0.0f), grey(0.5f) + rho * 0.5f, 0.03f));

        assert(cell.unsureShare == 0.0f);

        // Nothing known round the hits because they lie past the grid: guessed, but nothing to wait for.
        const SkyCell guessed = bakeSkyCell(level, point, manyRays, 30.0f, 0.5f, marks, albedo, unknownAt);
        assert(nearColour(skyLightFacing(guessed.visibility, guessed.bounce, down, 0.0f), rho, 0.03f));
        assert(guessed.unsureShare == 0.0f);

        // Nothing known because nothing round them is baked yet: guessed, and worth baking again later.
        const SkyCell waiting = bakeSkyCell(level, point, manyRays, 30.0f, 0.5f, marks, albedo, [](const Vec3&, const Vec3&, const Vec3&) {
            SkySample sample;
            sample.missing = 1.0f;
            return sample;
        });
        assert(waiting.unsureShare == 1.0f);

        const SkyCell plain = bakeSkyCell(level, point, manyRays, 30.0f, 0.5f, marks);
        for (const SkyVisibility& channel : plain.bounce) {
            assert(channel.open == 0.0f && channel.lean.x == 0.0f && channel.lean.y == 0.0f && channel.lean.z == 0.0f);
        }
    }

    /*
    * A closed room with nothing known round its hits: its own open share is none, so the guess lights nothing and no
    * bounce comes back. A point inside a block standing alone meets only the backs of its triangles, which bring none
    * either (inside a wall of a level, some rays leave through faces that touch others, the floor's or the roof's, and
    * meet their fronts; such a point is buried and never read).
    */
    void testNoBounceInTheDarkOrInWalls() {
        StaticLevel closed = roomWithADoor();
        closed.add(block(Vec3{3.0f, 0.0f, -0.5f}, Vec3{3.2f, 2.0f, 0.5f})); // the door shut
        const std::vector<Vec3> albedo = albedoOf(closed, grey(0.9f));
        GridMarks marks;

        const SkyCell inside = bakeSkyCell(closed, Vec3{1.5f, 1.0f, 0.1f}, manyRays, 30.0f, 0.5f, marks, albedo, unknownAt);
        assert(inside.visibility.open == 0.0f);
        for (const SkyVisibility& channel : inside.bounce) {
            assert(channel.open == 0.0f);
        }

        StaticLevel alone;
        alone.add(block(Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}));
        const SkyCell inBlock = bakeSkyCell(alone, Vec3{0.1f, 0.2f, -0.3f}, manyRays, 30.0f, 0.5f, marks, albedoOf(alone, grey(0.9f)), openSkyAt);
        assert(inBlock.state == SkyCellState::Buried);
        for (const SkyVisibility& channel : inBlock.bounce) {
            assert(channel.open == 0.0f);
        }
    }

    void testTheBudget() {
        assert(skyCellsForBudget(0.001, 1e-5, 1000) == 100);
        assert(skyCellsForBudget(0.001, 1e-5, 40) == 40);
        assert(skyCellsForBudget(0.001, 1.0, 40) == 1);
        assert(skyCellsForBudget(0.001, 0.0, 40) == 40);
        assert(skyCellsForBudget(0.0, 1e-5, 40) == 0);
        assert(skyCellsForBudget(0.001, 1e-5, 0) == 0);

        // Never fewer than a round of jobs for every core (when that many wait): a batch of one runs on one core and
        // measures a point sixteen times dearer, which shrank the next budget until the bake crawled at a point a frame.
        assert(skyCellsForBudget(0.001, 1.0, 400, 128) == 128);
        assert(skyCellsForBudget(0.001, 1.0, 40, 128) == 40);
        assert(skyCellsForBudget(0.001, 1e-5, 1000, 128) == 128);
        assert(skyCellsForBudget(0.001, 1e-6, 5000, 128) == 1000);
        assert(skyCellsForBudget(0.0, 1e-5, 40, 128) == 0);
        assert(skyMinimumBatch(16) == 16 * static_cast<int>(skyBakePointsPerJob) && skyMinimumBatch(0) == static_cast<int>(skyBakePointsPerJob));
    }

    /*
    * What a frame bakes: a capture everything at full rays, refinement included; a fresh grid every waiting point at
    * once, quickly when quick rays are fewer; otherwise never more than the budget, however far behind a move left it:
    * at full rays while the budget covers what waits, waiting points first and refinement with what is left, and past
    * it the quick budget's worth at the quick rays.
    */
    void testPlanningAFrame() {
        const auto same = [](const SkyBakePlan& a, const SkyBakePlan& b) {
            return a.waiting == b.waiting && a.quick == b.quick && a.refine == b.refine;
        };

        assert(same(planSkyBake(300, 500, false, true, 10, 40, true), SkyBakePlan{300, false, 500}));
        assert(same(planSkyBake(1000, 0, true, false, 10, 40, true), SkyBakePlan{1000, true, 0}));
        assert(same(planSkyBake(1000, 0, true, false, 10, 40, false), SkyBakePlan{1000, false, 0}));

        // Far behind after a fast move: the quick budget, no long frame.
        assert(same(planSkyBake(60000, 200, false, false, 10, 40, true), SkyBakePlan{40, true, 0}));
        assert(same(planSkyBake(11, 200, false, false, 10, 40, true), SkyBakePlan{11, true, 0}));
        assert(same(planSkyBake(60000, 200, false, false, 10, 40, false), SkyBakePlan{10, false, 0}));

        // Keeping up: full rays, then refinement.
        assert(same(planSkyBake(4, 500, false, false, 10, 40, true), SkyBakePlan{4, false, 6}));
        assert(same(planSkyBake(10, 500, false, false, 10, 40, true), SkyBakePlan{10, false, 0}));
        assert(same(planSkyBake(0, 3, false, false, 10, 40, true), SkyBakePlan{0, false, 3}));
        assert(same(planSkyBake(0, 0, false, false, 10, 40, true), SkyBakePlan{0, false, 0}));
    }

    // Few rays, so a whole cascade bakes quickly in a test.
    SkyVisibilitySettings quickGrid() {
        SkyVisibilitySettings settings;
        settings.cascades = 1;
        settings.rays = 4;
        return settings;
    }

    /*
    * The points round a place, described for the sky-points command: the queues' counts, then each cascade's eight
    * points with their state, whether they wait or are quick bakes still to refine, and their values.
    */
    void testDescribingPoints() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 8;
        settings.quickRays = 4;
        const int count = skyVisibilityCellCount(settings);
        SkyVisibilityGrid grid;
        const Vec3 centre{0.37f, 1.61f, -0.22f};
        grid.follow(centre, settings, 1);

        const std::string waiting = grid.describePoints(centre);
        assert(waiting.find("waiting " + std::to_string(count)) != std::string::npos);
        assert(waiting.find("cascade 0") != std::string::npos && waiting.find("cascade 1") == std::string::npos);
        assert(waiting.find("unknown, waiting") != std::string::npos);

        grid.bake(level, serialJobs(), count, true);
        const std::string quick = grid.describePoints(centre);
        assert(quick.find("waiting 0") != std::string::npos && quick.find("refining " + std::to_string(count)) != std::string::npos);
        assert(quick.find("quick") != std::string::npos && quick.find("open ") != std::string::npos);

        grid.refine(level, serialJobs(), count);
        assert(grid.describePoints(centre).find("quick") == std::string::npos);
    }

    /*
    * The points changed since the copy-up last asked, so it repacks only those: all after a fresh start, then each
    * batch baked or refined, and the plane a move brings in; nothing when nothing changed.
    */
    void testChangedPoints() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 8;
        settings.quickRays = 4;
        SkyVisibilityGrid grid;
        const Vec3 centre{0.37f, 1.61f, -0.22f};
        std::vector<int> changed;

        grid.follow(centre, settings, 1);
        assert(grid.takeChanges(changed));
        assert(!grid.takeChanges(changed) && changed.empty());

        assert(grid.bake(level, serialJobs(), 100, true) == 100);
        assert(!grid.takeChanges(changed) && changed.size() == 100);
        assert(grid.refine(level, serialJobs(), 30) == 30);
        assert(!grid.takeChanges(changed) && changed.size() == 30);

        grid.follow(centre + Vec3{settings.spacing, 0.0f, 0.0f}, settings, 1);
        assert(!grid.takeChanges(changed) && changed.size() == static_cast<std::size_t>(SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS));
        assert(!grid.takeChanges(changed) && changed.empty());
    }

    /*
    * The grid starts with every point waiting, bakes them in turn, and after a move of one spacing east bakes only the
    * plane that came in; a new level or a new shape starts it afresh, other settings do not.
    */
    void testTheGridFollowsTheCamera() {
        const StaticLevel level = roomWithADoor();
        const SkyVisibilitySettings settings = quickGrid();
        const int count = skyVisibilityCellCount(settings);
        SkyVisibilityGrid grid;
        const Vec3 centre{0.37f, 1.61f, -0.22f};

        assert(grid.follow(centre, settings, 1));
        assert(grid.getWaiting() == count && grid.getCells().size() == static_cast<std::size_t>(count));
        const std::uint64_t before = grid.getVersion();
        assert(grid.bake(level, serialJobs(), 100) == 100);
        assert(grid.getWaiting() == count - 100 && grid.getVersion() != before);

        assert(!grid.follow(centre, settings, 1));
        assert(grid.bake(level, serialJobs(), count) == count - 100);
        assert(grid.getWaiting() == 0);

        int unknown = 0;
        for (const SkyCell& cell : grid.getCells()) {
            unknown += cell.state == SkyCellState::Unknown ? 1 : 0;
        }
        assert(unknown == 0);

        // One spacing east: one plane comes in, left out until baked.
        assert(!grid.follow(centre + Vec3{settings.spacing, 0.0f, 0.0f}, settings, 1));
        assert(grid.getWaiting() == SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS);
        unknown = 0;
        for (const SkyCell& cell : grid.getCells()) {
            unknown += cell.state == SkyCellState::Unknown ? 1 : 0;
        }
        assert(unknown == SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS);

        // Moving back before it is baked brings back the plane it left, which the store kept: nothing waits.
        assert(!grid.follow(centre, settings, 1));
        assert(grid.getWaiting() == 0);

        SkyVisibilitySettings looks = settings;
        looks.ambientFloor = 0.06f;
        looks.bakeMilliseconds = 4.0f;
        assert(!grid.follow(centre, looks, 1));
        assert(grid.follow(centre, looks, 2) && grid.getWaiting() == count);
        SkyVisibilitySettings shape = looks;
        shape.rayLength = 10.0f;
        grid.bake(level, serialJobs(), count);
        assert(grid.follow(centre, shape, 2) && grid.getWaiting() == count);
    }

    // Whether a point holds exactly what baking it alone at this many rays gives.
    bool bakedWith(const SkyVisibilityGrid& grid, const StaticLevel& level, const SkyVisibilitySettings& settings, int cell, int rays) {
        const ProbeCascade& cascade = grid.getCascades()[static_cast<std::size_t>(probeCascadeHolding(grid.getCascades(), cell))];
        GridMarks marks;
        const SkyCell alone = bakeSkyCell(level, probePosition(cascade.volume, cell - cascade.firstProbe), rays, settings.rayLength,
                                          cascade.volume.spacing.x, marks);
        const GpuSkyCell a = packSkyCell(alone);
        const GpuSkyCell b = packSkyCell(grid.getCells()[static_cast<std::size_t>(cell)]);
        return a.words[0] == b.words[0] && a.words[1] == b.words[1] && a.words[2] == b.words[2] && a.words[3] == b.words[3];
    }

    /*
    * The two-step refill: a quick bake at the quick rays leaves every point waiting for refinement; refining bakes them
    * again at the full rays, in the order they came; points a move clears wait again instead, at full rays, and drop
    * out of refinement. With quick rays not fewer than the rays, a quick bake is a full one.
    */
    void testQuickBakesAreRefined() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 6;
        settings.quickRays = 2;
        const int count = skyVisibilityCellCount(settings);
        const Vec3 centre{0.37f, 1.61f, -0.22f};
        SkyVisibilityGrid grid;

        assert(grid.follow(centre, settings, 1));
        assert(grid.bake(level, serialJobs(), count, true) == count);
        assert(grid.getWaiting() == 0 && grid.getRefining() == count);
        assert(bakedWith(grid, level, settings, 0, 2) && bakedWith(grid, level, settings, count / 2, 2));

        const std::uint64_t before = grid.getVersion();
        assert(grid.refine(level, serialJobs(), 10) == 10);
        assert(grid.getRefining() == count - 10 && grid.getVersion() != before);

        for (int cell = 0; cell < 10; ++cell) {
            assert(bakedWith(grid, level, settings, cell, 6));
        }

        assert(bakedWith(grid, level, settings, 10, 2));

        // A move clears a plane: its points wait, and those still to be refined leave the queue.
        assert(!grid.follow(centre + Vec3{settings.spacing, 0.0f, 0.0f}, settings, 1));
        int cleared = 0;

        for (int cell = 10; cell < count; ++cell) {
            cleared += grid.getCells()[static_cast<std::size_t>(cell)].state == SkyCellState::Unknown ? 1 : 0;
        }

        assert(grid.getRefining() == count - 10 - cleared);
        assert(grid.refine(level, serialJobs(), count) == count - 10 - cleared);
        assert(grid.getRefining() == 0 && grid.getWaiting() == SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS);
        assert(grid.refine(level, serialJobs(), count) == 0);

        // The plane bakes at full rays, quick or not when its turn comes in the budget.
        assert(grid.bake(level, serialJobs(), count) == SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS);
        assert(grid.getRefining() == 0);

        for (int cell = 0; cell < count; cell += 997) {
            assert(bakedWith(grid, level, settings, cell, 6));
        }

        // Quick rays as many as the rays: nothing to refine.
        settings.quickRays = 6;
        SkyVisibilityGrid full;
        full.follow(centre, settings, 1);
        assert(full.bake(level, serialJobs(), 50, true) == 50 && full.getRefining() == 0);
        int checked = 0;

        for (int cell = 0; cell < count; ++cell) {
            if (full.getCells()[static_cast<std::size_t>(cell)].state != SkyCellState::Unknown) {
                assert(bakedWith(full, level, settings, cell, 6));
                ++checked;
            }
        }

        assert(checked == 50);
    }

    // A point's place in the world, from its number.
    Vec3 cellPosition(const SkyVisibilityGrid& grid, int cell) {
        const ProbeCascade& cascade = grid.getCascades()[static_cast<std::size_t>(probeCascadeHolding(grid.getCascades(), cell))];
        return probePosition(cascade.volume, cell - cascade.firstProbe);
    }

    /*
    * Behind a long move, what the budget bakes first: the coarsest cascade (a coarse point covers eight times what a
    * point a level denser does), and within it the points ahead of the motion or in front of the camera, nearest first.
    */
    void testFallingBehindBakesWhatMattersFirst() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.cascades = 2;
        settings.rays = 2;
        const int perCascade = skyVisibilityCellCount(settings) / 2;
        const Vec3 start{0.37f, 1.61f, -0.22f};
        const Vec3 centre = start + Vec3{200.0f, 0.0f, 0.0f};
        SkyVisibilityGrid grid;
        grid.follow(start, settings, 1);
        assert(!grid.follow(centre, settings, 1) && grid.getWaiting() == 2 * perCascade);

        const auto baked = [&grid](int cell) {
            return grid.getCells()[static_cast<std::size_t>(cell)].state != SkyCellState::Unknown;
        };

        // No view direction: the move east alone says what lies ahead.
        assert(grid.bake(level, serialJobs(), 500, false, Vec3{}) == 500);
        assert(grid.getWaiting() == 2 * perCascade - 500);
        float farthestBaked = 0.0f;

        for (int cell = 0; cell < 2 * perCascade; ++cell) {
            if (baked(cell)) {
                const Vec3 offset = cellPosition(grid, cell) - centre;
                assert(cell >= perCascade && offset.x >= 0.0f);
                farthestBaked = std::max(farthestBaked, offset.length());
            }
        }

        for (int cell = perCascade; cell < 2 * perCascade; ++cell) {
            const Vec3 offset = cellPosition(grid, cell) - centre;

            if (!baked(cell) && offset.x >= 0.0f) {
                assert(offset.length() >= farthestBaked - 1e-4f);
            }
        }

        // Standing still and looking north (-z): what is in front of the camera.
        assert(!grid.follow(centre, settings, 1));
        assert(grid.bake(level, serialJobs(), 500, false, Vec3{0.0f, 0.0f, -1.0f}) == 500);
        int northBaked = 0;

        for (int cell = 0; cell < 2 * perCascade; ++cell) {
            const Vec3 offset = cellPosition(grid, cell) - centre;

            if (baked(cell) && offset.x < 0.0f) {
                assert(cell >= perCascade && offset.z <= 0.0f);
                ++northBaked;
            }
        }

        assert(northBaked > 0);
    }

    bool sameWords(const SkyCell& a, const SkyCell& b) {
        const GpuSkyCell packedA = packSkyCell(a);
        const GpuSkyCell packedB = packSkyCell(b);
        return packedA.words[0] == packedB.words[0] && packedA.words[1] == packedB.words[1] && packedA.words[2] == packedB.words[2] &&
               packedA.words[3] == packedB.words[3];
    }

    /*
    * The store keeps points by cascade and lattice place, as the GPU reads them (so a kept point packs to exactly the
    * words it would have), with whether they were quick; past its capacity the one used longest ago goes first; places
    * past its coordinates' range are not kept; its capacity follows the megabytes.
    */
    void testTheStoreKeepsBakedPoints() {
        const StaticLevel level = roomWithADoor();
        GridMarks marks;
        const SkyCell inside = bakeSkyCell(level, Vec3{1.5f, 1.0f, 0.1f}, 16, 30.0f, 0.5f, marks);
        const SkyCell outside = bakeSkyCell(level, Vec3{8.0f, 1.0f, 0.1f}, 16, 30.0f, 0.5f, marks);

        SkyCellStore store;
        store.setCapacity(3);
        const ProbeGridCoord a{3, 2, 1};
        const ProbeGridCoord b{-4, 0, 7};
        const ProbeGridCoord c{100, 5, -9};
        store.keep(0, a, inside, false);
        store.keep(0, b, outside, true);
        store.keep(1, a, outside, false);
        assert(store.size() == 3);

        const std::optional<StoredSkyCell> foundA = store.find(0, a);
        assert(foundA && !foundA->unfinished && sameWords(foundA->cell, inside));
        const std::optional<StoredSkyCell> foundB = store.find(0, b);
        assert(foundB && foundB->unfinished && sameWords(foundB->cell, outside));
        assert(store.find(1, a) && sameWords(store.find(1, a)->cell, outside));
        assert(!store.find(0, c) && !store.find(2, a));

        // Finding is using: oldest (0, a), then (0, b), then (1, a). Touching (0, a) makes (0, b) the oldest.
        store.touch(0, a);
        store.keep(0, c, inside, false);
        assert(store.size() == 3 && !store.find(0, b) && store.find(0, a) && store.find(0, c) && store.find(1, a));

        // Keeping a place again replaces it (a refined point).
        store.keep(0, c, outside, false);
        assert(store.size() == 3 && sameWords(store.find(0, c)->cell, outside));

        // Out of the coordinates' range: not kept.
        store.keep(0, ProbeGridCoord{1 << 21, 0, 0}, inside, false);
        assert(!store.find(0, ProbeGridCoord{1 << 21, 0, 0}) && store.size() == 3);

        store.clear();
        assert(store.size() == 0 && !store.find(0, a));
        store.setCapacity(0);
        store.keep(0, a, inside, false);
        assert(store.size() == 0);

        SkyVisibilitySettings settings;
        assert(skyStoreCapacity(settings) == static_cast<std::size_t>(256.0 * 1024.0 * 1024.0 / skyStoreBytesPerPoint));
        settings.storeMegabytes = 0.0f;
        assert(skyStoreCapacity(settings) == 0);
    }

    /*
    * Coming back: points baked before a long move are found in the store when their places come back into the grid,
    * so nothing waits; quick ones go back to refinement. With the store off, or after a new level (which empties it),
    * they wait again.
    */
    void testComingBackFindsWhatWasBaked() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 6;
        settings.quickRays = 2;
        const int count = skyVisibilityCellCount(settings);
        const Vec3 home{0.37f, 1.61f, -0.22f};
        const Vec3 away = home + Vec3{200.0f, 0.0f, 0.0f};

        SkyVisibilityGrid grid;
        grid.follow(home, settings, 1);
        grid.bake(level, serialJobs(), count);
        const std::vector<SkyCell> baked(grid.getCells().begin(), grid.getCells().end());

        assert(!grid.follow(away, settings, 1) && grid.getWaiting() == count);
        const std::uint64_t before = grid.getVersion();
        assert(!grid.follow(home, settings, 1));
        assert(grid.getWaiting() == 0 && grid.getRefining() == 0 && grid.getVersion() != before);

        for (int cell = 0; cell < count; ++cell) {
            assert(sameWords(grid.getCells()[static_cast<std::size_t>(cell)], baked[static_cast<std::size_t>(cell)]));
        }

        // Halfway back after a partial bake away: only the places never baked wait.
        grid.follow(away, settings, 1);
        assert(grid.bake(level, serialJobs(), 100) == 100);
        grid.follow(home, settings, 1);
        assert(grid.getWaiting() == 0);
        grid.follow(away, settings, 1);
        assert(grid.getWaiting() == count - 100);

        // Quick points come back still to be refined.
        SkyVisibilityGrid quick;
        quick.follow(home, settings, 1);
        quick.bake(level, serialJobs(), count, true);
        quick.follow(away, settings, 1);
        assert(quick.getRefining() == 0);
        quick.follow(home, settings, 1);
        assert(quick.getWaiting() == 0 && quick.getRefining() == count);

        // A new level empties the store.
        assert(grid.follow(home, settings, 2) && grid.getWaiting() == count);
        grid.follow(away, settings, 2);
        grid.follow(home, settings, 2);
        assert(grid.getWaiting() == count);

        // The store off: nothing comes back.
        settings.storeMegabytes = 0.0f;
        SkyVisibilityGrid forgetful;
        forgetful.follow(home, settings, 1);
        forgetful.bake(level, serialJobs(), count);
        forgetful.follow(away, settings, 1);
        forgetful.follow(home, settings, 1);
        assert(forgetful.getWaiting() == count);
    }

    /*
    * Points baked before the grid round their hits was: queued to be baked again once, when it is, as quick ones are;
    * baking them again does not queue them a third time, nor does the store bring them back as finished.
    */
    void testPointsBakedTooEarlyAreBakedAgain() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 8;
        const std::vector<Vec3> albedo = albedoOf(level, grey(0.6f));
        const int count = skyVisibilityCellCount(settings);
        const Vec3 centre{0.37f, 1.61f, -0.22f};

        SkyVisibilityGrid grid;
        grid.follow(centre, settings, 1);
        assert(grid.bake(level, serialJobs(), count, false, Vec3{}, albedo) == count);
        const int early = grid.getRefining();
        assert(early > 0 && early <= count);

        assert(grid.refine(level, serialJobs(), count, albedo) == early);
        assert(grid.getRefining() == 0);

        // Without albedo there is no bounce to be unsure of.
        SkyVisibilityGrid plain;
        plain.follow(centre, settings, 1);
        plain.bake(level, serialJobs(), count);
        assert(plain.getRefining() == 0);
    }

    // Many threads bake exactly what one does.
    void testBakingIsTheSameOnEveryCore() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 16;
        const Vec3 centre{0.37f, 1.61f, -0.22f};

        // Two bakes, the second reading what the first left round its hits.
        const std::vector<Vec3> albedo = albedoOf(level, Vec3{0.7f, 0.5f, 0.3f});
        SkyVisibilityGrid alone;
        alone.follow(centre, settings, 1);
        alone.bake(level, serialJobs(), 5000, false, Vec3{}, albedo);
        alone.bake(level, serialJobs(), 5000, false, Vec3{}, albedo);

        JobSystem jobs{4};
        SkyVisibilityGrid together;
        together.follow(centre, settings, 1);
        together.bake(level, jobs, 5000, false, Vec3{}, albedo);
        together.bake(level, jobs, 5000, false, Vec3{}, albedo);

        for (std::size_t cell = 0; cell < alone.getCells().size(); ++cell) {
            const GpuSkyCell a = packSkyCell(alone.getCells()[cell]);
            const GpuSkyCell b = packSkyCell(together.getCells()[cell]);
            for (std::size_t word = 0; word < std::size(a.words); ++word) {
                assert(a.words[word] == b.words[word]);
            }
        }
    }
}

void testSkyVisibility() {
    testAnOpenFieldLightsTheFloorFully();
    testAClosedBoxIsDark();
    testTheShareStaysWithinItsRange();
    testSeenFacingAndAlong();
    testACornerSeesWithinItsReach();
    testAPointPacksForTheShaders();
    testTheCascadesNestRoundTheCamera();
    testReadingTheGrid();
    testPointsInWallsOrBehindThemAreLeftOut();
    testAFloorReadsWhatIsAboveIt();
    testTheSettings();
    testThePresets();
    testBakingAPointOverAFloor();
    testBakingARoom();
    testTheLightOnAHit();
    testBounceOverAFloor();
    testNoBounceInTheDarkOrInWalls();
    testTheBudget();
    testPlanningAFrame();
    testDescribingPoints();
    testChangedPoints();
    testTheGridFollowsTheCamera();
    testQuickBakesAreRefined();
    testFallingBehindBakesWhatMattersFirst();
    testTheStoreKeepsBakedPoints();
    testComingBackFindsWhatWasBaked();
    testPointsBakedTooEarlyAreBakedAgain();
    testBakingIsTheSameOnEveryCore();
}
