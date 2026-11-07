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

    /*
    * An open field: the sky above, the ground below. The bands are a = 1/2 and b = 3/4 up, so a floor gets all of the
    * sky's light (as the flat ambient assumed for every surface), a wall half and a ceiling none.
    */
    void testAnOpenFieldLightsTheFloorFully() {
        const SkyVisibility field = bandsOf([](const Vec3& direction) { return direction.y > 0.0f; });
        assert(near(field.open, 0.5f, 0.01f));
        assert(near(field.lean.y, 0.75f, 0.02f) && near(field.lean.x, 0.0f, 0.02f) && near(field.lean.z, 0.0f, 0.02f));

        assert(near(skyVisibilityFacing(field, up, 0.0f), 1.0f, 0.02f));
        assert(near(skyVisibilityFacing(field, east, 0.0f), 0.5f, 0.02f));
        assert(near(skyVisibilityFacing(field, down, 0.0f), 0.0f, 0.02f));
        assert(near(skyVisibilityInAir(field, 0.0f), 0.5f, 0.01f));
        assert(near(skyVisibilityAlong(field, up, 0.0f), 1.0f, 0.02f));
    }

    // A closed box sees no sky, and its walls none either, so nothing comes back: it is dark whatever the bounce share;
    // with everything open, every surface gets all.
    void testAClosedBoxIsDark() {
        const SkyVisibility box = bandsOf([](const Vec3&) { return false; });
        const SkyVisibility open = bandsOf([](const Vec3&) { return true; });

        for (const Vec3& normal : {up, down, east}) {
            assert(skyVisibilityFacing(box, normal, 0.0f) == 0.0f);
            assert(skyVisibilityFacing(box, normal, 0.25f) == 0.0f);
            assert(near(skyVisibilityFacing(open, normal, 0.25f), 1.0f, 1e-4f));
        }

        assert(skyVisibilityInAir(box, 0.3f) == 0.0f);

        // With a floor on how lit blockers count, a closed room keeps that much of the bounce: dim, not black.
        assert(near(skyVisibilityFacing(box, up, 0.25f, 0.1f), 0.025f, 1e-6f));
        assert(near(skyVisibilityInAir(box, 0.3f, 0.1f), 0.03f, 1e-6f));
        assert(skyBlockersLit(box, 0.1f) == 0.1f && near(skyBlockersLit(open, 0.1f), 1.0f, 1e-6f));

        // Under an open sky the blocked half (the ground) brings back the whole bounce share: a ceiling gets beta, a wall
        // half the sky and beta of the other half.
        const SkyVisibility field = bandsOf([](const Vec3& direction) { return direction.y > 0.0f; });
        assert(near(skyVisibilityFacing(field, down, 0.25f), 0.25f, 0.02f));
        assert(near(skyVisibilityFacing(field, east, 0.25f), 0.5f + 0.25f * 0.5f, 0.02f));
    }

    // Two bands blur sharp edges and can overshoot: the sky seen is kept within 0..1 before the bounce is added.
    void testTheShareStaysWithinItsRange() {
        const SkyVisibility ringing{0.2f, Vec3{0.0f, 0.9f, 0.0f}};
        const float blockersLit = 0.4f; // twice the open share
        assert(near(skyVisibilityFacing(ringing, down, 0.25f), 0.25f * blockersLit, 1e-6f));
        assert(skyVisibilityAlong(ringing, up, 0.25f) == 1.0f);

        // f + beta g (1 - f), with f = a + 2/3 b . n.
        const float seen = 0.2f + 0.6f;
        assert(near(skyVisibilityFacing(ringing, up, 0.5f), seen + 0.5f * blockersLit * (1.0f - seen), 1e-5f));
        assert(near(skyVisibilityInAir(ringing, 0.5f), 0.2f + 0.5f * blockersLit * 0.8f, 1e-5f));
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

        const SkyCell back = unpackSkyCell(packSkyCell(cell));
        assert(back.visibility.open == 0.375f && back.visibility.lean.x == -0.25f && back.visibility.lean.y == 0.5f && back.visibility.lean.z == 0.125f);
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

    SkyCell bakedCell(const SkyVisibility& visibility) {
        SkyCell cell;
        cell.visibility = visibility;
        cell.state = SkyCellState::Baked;
        return cell;
    }

    // Nothing baked yet reads as the open sky, as before the grid; points that all agree read as that.
    void testReadingTheGrid() {
        SkyVisibilitySettings settings;
        const Vec3 centre{0.37f, 1.61f, -0.22f};
        const std::vector<ProbeCascade> cascades = skyVisibilityCascades(centre, settings);
        const int count = skyVisibilityCellCount(settings);

        const std::vector<SkyCell> unknown(static_cast<std::size_t>(count));
        const SkyVisibility open = sampleSkyVisibility(cascades, unknown, settings.lookupBias, centre, up, up);
        assert(open.open == 1.0f && open.lean.x == 0.0f && open.lean.y == 0.0f && open.lean.z == 0.0f);

        const SkyVisibility room{0.1f, Vec3{0.05f, 0.02f, 0.0f}};
        const std::vector<SkyCell> same = placedCells(cascades, count, [&](const Vec3&) { return bakedCell(room); });
        const SkyVisibility read = sampleSkyVisibility(cascades, same, settings.lookupBias, centre, up, up);
        assert(near(read.open, room.open, 1e-5f) && near(read.lean.x, room.lean.x, 1e-5f) && near(read.lean.y, room.lean.y, 1e-5f));

        // Far past the last cascade, the open sky again.
        const SkyVisibility far = sampleSkyVisibility(cascades, same, settings.lookupBias, centre + Vec3{500.0f, 0.0f, 0.0f}, up, up);
        assert(far.open == 1.0f);
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
        assert(near(sampleSkyVisibility(cascades, buried, settings.lookupBias, centre, up, up).open, 1.0f, 1e-5f));

        const std::vector<SkyCell> walled = placedCells(cascades, count, [&](const Vec3& position) {
            SkyCell cell = bakedCell(position.x < centre.x ? dark : sky);
            if (position.x < centre.x) {
                cell.reach[0] = 0.0f;
            }
            return cell;
        });
        assert(near(sampleSkyVisibility(cascades, walled, settings.lookupBias, centre, up, up).open, 1.0f, 1e-5f));

        // Without the wall both sides count.
        const std::vector<SkyCell> both = placedCells(cascades, count, [&](const Vec3& position) { return bakedCell(position.x < centre.x ? dark : sky); });
        const float mixed = sampleSkyVisibility(cascades, both, settings.lookupBias, centre, up, up).open;
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

        assert(sampleSkyVisibility(cascades, cells, settings.lookupBias, floorPoint, up, up).open > 0.75f);
        assert(sampleSkyVisibility(cascades, cells, settings.lookupBias, floorPoint, down, down).open < 0.3f);
    }

    void testTheSettings() {
        const SkyVisibilitySettings defaults;
        assert(defaults.enabled && defaults.bounceShare == 0.25f && defaults.spacing == 0.5f && defaults.cascades == 2);
        assert(defaults.rays == 64 && defaults.rayLength == 30.0f && defaults.bakeMilliseconds == 1.0f && !defaults.debugView);
        assert(defaults.blockersLitFloor == 0.1f);
        validateSkyVisibility(defaults);

        SkyVisibilitySettings bad;
        bad.bounceShare = 1.5f;
        assert(rejects(bad));
        bad = {};
        bad.blockersLitFloor = -0.1f;
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
        other.bounceShare = 0.5f;
        other.lookupBias = 0.1f;
        other.bakeMilliseconds = 3.0f;
        other.debugView = true;
        other.blockersLitFloor = 0.3f;
        assert(sameSkyVisibilityGrid(defaults, other));
        other.rays = 32;
        assert(!sameSkyVisibilityGrid(defaults, other));

        // Reachable by key from the command line and the settings file.
        RenderSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings =
            applySettings({{"skyVisibility.bounceShare", "0.4"}, {"skyVisibility.enabled", "false"}, {"skyVisibility.debugView", "true"}, {"skyVisibility.blockersLitFloor", "0.2"}}, walk,
                          StartupFields::Keep);
        assert(warnings.empty() && settings.skyVisibility.bounceShare == 0.4f && !settings.skyVisibility.enabled && settings.skyVisibility.debugView);
        assert(settings.skyVisibility.blockersLitFloor == 0.2f);

        // The shaders' options: nothing while not read.
        const GpuSkyVisibilityOptions off = packSkyVisibilityOptions(defaults, 0);
        assert(off.values[0] == 0.0f && off.values[1] == 0.0f && off.values[2] == 0.0f && off.values[3] == 0.0f && off.more[0] == 0.0f);
        const GpuSkyVisibilityOptions on = packSkyVisibilityOptions(other, 2);
        assert(on.values[0] == 2.0f && on.values[1] == 0.5f && on.values[2] == 0.1f && on.values[3] == 1.0f);
        assert(on.more[0] == 0.3f);
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
        assert(near(skyVisibilityFacing(cell.visibility, up, 0.0f), 1.0f, 0.03f));
        assert(near(skyVisibilityFacing(cell.visibility, down, 0.0f), 0.0f, 0.03f));
        assert(near(cell.reach[3], 0.2f, 0.01f));
        assert(cell.reach[0] == 1.0f && cell.reach[1] == 1.0f && cell.reach[2] == 1.0f && cell.reach[4] == 1.0f && cell.reach[5] == 1.0f);

        // A ray running past its length counts as open: a roof 40 m up does not shade a point given 30 m.
        StaticLevel roofed = flatLevel();
        roofed.add(block(Vec3{-50.0f, 40.0f, -50.0f}, Vec3{50.0f, 41.0f, 50.0f}));
        const SkyCell under = bakeSkyCell(roofed, Vec3{0.3f, 1.0f, 0.2f}, manyRays, 30.0f, 0.5f, marks);
        assert(near(skyVisibilityFacing(under.visibility, up, 0.0f), 1.0f, 0.03f));
        const SkyCell roofSeen = bakeSkyCell(roofed, Vec3{0.3f, 1.0f, 0.2f}, manyRays, 100.0f, 0.5f, marks);
        assert(skyVisibilityFacing(roofSeen.visibility, up, 0.0f) < 0.6f);
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
        const float towardsDoor = skyVisibilityFacing(inside.visibility, east, 0.0f);
        const float awayFromDoor = skyVisibilityFacing(inside.visibility, Vec3{-1.0f, 0.0f, 0.0f}, 0.0f);
        assert(inside.visibility.open < 0.1f);
        assert(towardsDoor > awayFromDoor);
        assert(skyVisibilityFacing(inside.visibility, up, 0.0f) < 0.15f);

        const SkyCell inWall = bakeSkyCell(level, Vec3{-3.1f, 1.0f, 0.1f}, manyRays, 30.0f, 0.5f, marks);
        assert(inWall.state == SkyCellState::Buried);

        // Beside the west wall's inside face, 0.1 m off: its reach west is a fifth of a spacing.
        const SkyCell byWall = bakeSkyCell(level, Vec3{-2.9f, 1.0f, 0.1f}, 8, 30.0f, 0.5f, marks);
        assert(near(byWall.reach[1], 0.2f, 0.01f) && byWall.reach[0] == 1.0f);
    }

    void testTheBudget() {
        assert(skyCellsForBudget(0.001, 1e-5, 1000) == 100);
        assert(skyCellsForBudget(0.001, 1e-5, 40) == 40);
        assert(skyCellsForBudget(0.001, 1.0, 40) == 1);
        assert(skyCellsForBudget(0.001, 0.0, 40) == 40);
        assert(skyCellsForBudget(0.0, 1e-5, 40) == 0);
        assert(skyCellsForBudget(0.001, 1e-5, 0) == 0);
    }

    // Few rays, so a whole cascade bakes quickly in a test.
    SkyVisibilitySettings quickGrid() {
        SkyVisibilitySettings settings;
        settings.cascades = 1;
        settings.rays = 4;
        return settings;
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

        // Moving back before it is baked brings in the other plane, each point waiting once.
        assert(!grid.follow(centre, settings, 1));
        assert(grid.getWaiting() == SKY_VISIBILITY_HIGH * SKY_VISIBILITY_ACROSS);

        SkyVisibilitySettings looks = settings;
        looks.bounceShare = 0.6f;
        looks.bakeMilliseconds = 4.0f;
        assert(!grid.follow(centre, looks, 1));
        assert(grid.follow(centre, looks, 2) && grid.getWaiting() == count);
        SkyVisibilitySettings shape = looks;
        shape.rayLength = 10.0f;
        grid.bake(level, serialJobs(), count);
        assert(grid.follow(centre, shape, 2) && grid.getWaiting() == count);
    }

    // Many threads bake exactly what one does.
    void testBakingIsTheSameOnEveryCore() {
        const StaticLevel level = roomWithADoor();
        SkyVisibilitySettings settings = quickGrid();
        settings.rays = 16;
        const Vec3 centre{0.37f, 1.61f, -0.22f};

        SkyVisibilityGrid alone;
        alone.follow(centre, settings, 1);
        alone.bake(level, serialJobs(), 5000);

        JobSystem jobs{4};
        SkyVisibilityGrid together;
        together.follow(centre, settings, 1);
        together.bake(level, jobs, 5000);

        for (std::size_t cell = 0; cell < alone.getCells().size(); ++cell) {
            const GpuSkyCell a = packSkyCell(alone.getCells()[cell]);
            const GpuSkyCell b = packSkyCell(together.getCells()[cell]);
            for (int word = 0; word < 4; ++word) {
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
    testTheBudget();
    testTheGridFollowsTheCamera();
    testBakingIsTheSameOnEveryCore();
}
