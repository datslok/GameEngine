#include "engine/golden.h"
#include "games/lightlab/light_lab_game.h"
#include "games/lightlab/light_lab_level.h"
#include "gameplay/first_person_body.h"
#include "gameplay/flashlight.h"
#include "input/input.h"
#include "math/angles.h"
#include "math/transform.h"
#include "physics/character_controller.h"
#include "physics/overlap.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/probe_cascades.h"
#include "render/gpu/probe_relocation.h"
#include "scene/lighting.h"
#include "scene/model_renderer.h"
#include "scene/sky_object.h"
#include "scene/sun.h"

#include "demo_helpers.h"
#include "probe_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {
    // A fresh light lab, initialised headless.
    struct Lab {
        World world;
        AssetManager assets;
        LightLabGame game{ControlMode::FreeCamera, true};

        Lab() {
            game.onInit(world, assets);
        }
    };

    // The box grown by a margin on every side.
    Aabb grown(const Aabb& box, float margin) {
        return Aabb{box.minimum - Vec3{margin, margin, margin}, box.maximum + Vec3{margin, margin, margin}};
    }

    /*
    * The ground and the four groups of blocks are the static level, which the light probes move off; everything else
    * (the lamp's fixture, the sun's disc, the player's body) is not.
    */
    void testTheLevelIsStatic() {
        Lab lab;
        int staticModels = 0;
        int others = 0;

        lab.world.each<ModelRenderer>([&](Entity, ModelRenderer& renderer) { renderer.isStatic ? ++staticModels : ++others; });

        assert(staticModels == 5);
        assert(others > 0);
    }

    // Each tower stands at its doubling distance, sized in proportion to it, and no tower overlaps another's bearing.
    void testTowersDoubleAndFanOut() {
        const LightLabLayout layout = buildLightLabLayout();
        assert(layout.towers.size() == labTowerDistances.size());
        float previousRightEdge = -fullTurnRadians;

        for (std::size_t index = 0; index < layout.towers.size(); ++index) {
            const LabBlock& tower = layout.towers[index];
            const float distance = labTowerDistances[index];
            const float horizontal = std::sqrt(tower.centre.x * tower.centre.x + tower.centre.z * tower.centre.z);
            assert(near(horizontal, distance, 1e-3f * distance));
            assert(near(tower.halfSize.x * 2.0f, distance * labTowerWidthPerMetre, 1e-5f * distance));
            assert(near(tower.halfSize.y * 2.0f, distance * labTowerHeightPerMetre, 1e-5f * distance));
            assert(near(tower.centre.y, tower.halfSize.y, 1e-4f));

            // Its bearing from north (-z), east positive, and how wide it looks.
            const float bearing = std::atan2(tower.centre.x, -tower.centre.z);
            const float halfAngle = std::atan(tower.halfSize.x * std::sqrt(2.0f) / distance);
            assert(bearing - halfAngle > previousRightEdge);
            previousRightEdge = bearing + halfAngle;
        }
    }

    // The fence: evenly spaced thin posts along one line, and a rail joining them.
    void testFence() {
        const LightLabLayout layout = buildLightLabLayout();
        const int posts = static_cast<int>(std::lround((labFirstPostZ - labLastPostZ) / labPostSpacing)) + 1;
        assert(static_cast<int>(layout.fence.size()) == posts + 1);

        for (int index = 0; index < posts; ++index) {
            const LabBlock& post = layout.fence[static_cast<std::size_t>(index)];
            assert(post.centre.x == labFenceX);
            assert(near(post.centre.z, labFirstPostZ - labPostSpacing * static_cast<float>(index), 1e-3f));
            assert(post.halfSize.x == labPostHalfWidth);
        }

        const Aabb rail = layout.fence.back().bounds();
        assert(near(rail.maximum.z, labFirstPostZ, 0.1f));
        assert(near(rail.minimum.z, labLastPostZ, 0.1f));
    }

    // The forest is the same every time, all of it on the ground, outside the keep-clear areas.
    void testForest() {
        const LightLabLayout first = buildLightLabLayout();
        const LightLabLayout second = buildLightLabLayout();
        assert(first.trees.size() == static_cast<std::size_t>(labTreeCount) * 2);
        assert(first.trees.size() == second.trees.size());

        const float keepClearSlope = std::tan(radiansFromDegrees(labViewKeepClearHalfAngleDegrees));

        for (std::size_t index = 0; index < first.trees.size(); ++index) {
            const LabBlock& block = first.trees[index];
            assert(block.centre.x == second.trees[index].centre.x && block.centre.z == second.trees[index].centre.z);

            const float distance = std::sqrt(block.centre.x * block.centre.x + block.centre.z * block.centre.z);
            assert(distance >= labForestInnerRadius && distance <= labForestOuterRadius);
            assert(!(block.centre.z < 0.0f && std::abs(block.centre.x) < keepClearSlope * -block.centre.z + labViewKeepClearMargin));
            assert(!overlaps(block.bounds(), grown(labSunlitRoom, labBuildingKeepClear)));
            assert(!overlaps(block.bounds(), grown(labBackRoom, labBuildingKeepClear)));
        }

        // Trunks and crowns alternate.
        assert(first.trees[0].surface == LabSurface::Bark && first.trees[1].surface == LabSurface::Leaves);
    }

    // The rooms are empty inside and the openings go through the walls: no block in any of them.
    void testBuildingOpenings() {
        const LightLabLayout layout = buildLightLabLayout();
        const float shrink = -0.01f;

        for (const LabBlock& block : layout.building) {
            for (const Aabb& space : {labSunlitRoom, labBackRoom, labWindow, labInnerDoorway, labFrontDoor}) {
                assert(!overlaps(block.bounds(), grown(space, shrink)));
            }
        }

        // Just one red panel, inside the sunlit room's east wall.
        int redPanels = 0;
        for (const LabBlock& block : layout.building) {
            if (block.surface == LabSurface::RedPaint) {
                ++redPanels;
                assert(near(block.bounds().maximum.x, labSunlitRoom.maximum.x + labRedPanelThickness, 1e-4f));
            }
        }
        assert(redPanels == 1);
    }

    /*
    * The densest cascade's lattice puts a layer of probes inside the window wall (0.3 m thick) and two columns exactly
    * on the planes of the window's sides, where the wall's boxes touch: two faces on top of each other inside the solid.
    * Relocated against the level's own triangles, as the GPU does it, every probe in and round that wall either ends
    * outside every block or is off. Rays starting on such a seam used to meet its faces at distance 0, so the probe
    * took itself for one in the open beside a surface and stayed buried, lending the wall dark and sunlit patches.
    */
    void testProbesLeaveTheWindowWall() {
        Lab lab;
        const LightLabLayout layout = buildLightLabLayout();
        const ProbeCascadeLayout cascades = probeCascadeLayout(GlobalIlluminationSettings{});
        const float spacing = cascades.spacing;
        const float offset = probeLatticeOffset(cascades);
        const Vec3 spacings{spacing, spacing, spacing};
        const ProbeRayTrace trace = [&](const Vec3& origin, const Vec3& direction) {
            return traceRelocationRay(lab.game.getLevel(), origin, direction);
        };

        // The wall's slab grown by a cell each way, so every probe whose cells it reaches is checked.
        const Aabb region = grown(Aabb{Vec3{labBackRoom.minimum.x - labWallThickness, 0.0f, labBackRoom.minimum.z - labWallThickness},
                                       Vec3{labBackRoom.minimum.x, labRoomHeight, labSunlitRoom.maximum.z + labWallThickness}},
                                  spacing);
        const auto firstPlane = [&](float minimum) { return std::ceil((minimum - offset) / spacing) * spacing + offset; };
        int checked = 0;

        for (float x = firstPlane(region.minimum.x); x <= region.maximum.x; x += spacing) {
            for (float y = firstPlane(std::max(region.minimum.y, 0.0f)); y <= region.maximum.y; y += spacing) {
                for (float z = firstPlane(region.minimum.z); z <= region.maximum.z; z += spacing) {
                    const Vec3 grid{x, y, z};
                    const GpuProbeRelocation relocation = relocateProbe(grid, spacings, probeRelocationIterations, trace);
                    ++checked;

                    if (relocation.off != 0.0f) {
                        continue;
                    }

                    const Vec3 probe = grid + Vec3{relocation.offset[0], relocation.offset[1], relocation.offset[2]};
                    for (const LabBlock& block : layout.building) {
                        assert(!grown(block.bounds(), -probeRelocationMinDistance).contains(probe));
                    }
                }
            }
        }

        assert(checked > 300);
    }

    /*
    * The 4 m cascade has one probe inside the back room, its grid point low over the floor. Relocated against the level,
    * it rises off the floor and stays well inside the room, more than 2 m from the open front door: walked towards the
    * farthest surface it saw (out through the door) it ended 0.85 m from the doorway, and the room seen from afar glowed
    * with the sunlit outdoors it saw there.
    */
    void testTheBackRoomsCoarseProbeKeepsAwayFromTheDoor() {
        Lab lab;
        const ProbeRayTrace trace = [&](const Vec3& origin, const Vec3& direction) {
            return traceRelocationRay(lab.game.getLevel(), origin, direction);
        };
        const ProbeCascadeLayout cascades = probeCascadeLayout(GlobalIlluminationSettings{});
        const float spacing = probeCascadeSpacing(cascades, 3);
        const Vec3 grid{-19.75f, 0.25f, 8.25f};
        assert(spacing == 4.0f && labBackRoom.contains(grid));

        const GpuProbeRelocation relocation = relocateProbe(grid, Vec3{spacing, spacing, spacing}, probeRelocationIterations, trace);
        const Vec3 probe = grid + Vec3{relocation.offset[0], relocation.offset[1], relocation.offset[2]};
        assert(relocation.off == 0.0f && labBackRoom.contains(probe));
        assert(probe.y > grid.y + 0.4f);
        assert((probe - labFrontDoor.centre()).length() > 2.0f);
    }

    // At the default afternoon, the sunbeam through the middle of the window lands on the sunlit room's floor.
    void testSunbeamThroughTheWindow() {
        Lab lab;
        const LightLabSunSettings& settings = lab.game.getSunSettings();
        const Vec3 towardsSun = directionTowardsSunOn(settings.latitudeDegrees, settings.dayOfYear, settings.solarHours);
        assert(towardsSun.x < -0.5f && towardsSun.y > 0.2f);

        const Vec3 windowMiddle = labWindow.centre();
        const Ray beam{windowMiddle + towardsSun * 5.0f, towardsSun * -1.0f};
        const std::optional<LevelHit> hit = raycast(lab.game.getLevel(), beam);
        assert(hit);
        assert(near(hit->hit.point.y, labFloorTop, 1e-3f));
        assert(grown(labSunlitRoom, 1e-3f).contains(hit->hit.point));

        // The sun component follows the settings, and the sky shows behind everything (the atmosphere's).
        assert(collectLighting(lab.world, 1.0f).atmosphere.has_value());
        const DirectionalLight& sun = lab.world.get<DirectionalLight>(lab.game.getSun());
        assert(near(sun.direction.normalized().dot(towardsSun), -1.0f, 1e-5f));
        assert(sun.castsShadows);
    }

    // Walk the body west for some seconds from a start, through the level; returns where its feet end up.
    Vec3 walkWest(Lab& lab, const Vec3& start, float seconds) {
        const Entity body = lab.game.getFirstPersonBody();
        placeFirstPersonBody(lab.world, body, start);
        const int ticks = static_cast<int>(seconds / tickSeconds);

        for (int tick = 0; tick < ticks; ++tick) {
            lab.world.get<CharacterController>(body).wishVelocity = Vec3{-4.0f, 0.0f, 0.0f};
            updateFirstPersonBodies(lab.world, lab.game.getLevel(), tickSeconds);
        }

        return lab.world.get<Transform>(body).position;
    }

    // The front door lets you in, the far wall stops you; a wall without a door stops you outside.
    void testWalkingIn() {
        Lab lab;
        const float doorZ = labFrontDoor.centre().z;

        const Vec3 throughDoor = walkWest(lab, Vec3{-14.0f, 0.0f, doorZ}, 4.0f);
        assert(grown(labBackRoom, 0.01f).contains(throughDoor));
        assert(throughDoor.x > labBackRoom.minimum.x);

        const Vec3 blocked = walkWest(lab, Vec3{-14.0f, 0.0f, labSunlitRoom.centre().z}, 4.0f);
        assert(blocked.x > labSunlitRoom.maximum.x);
    }

    // Without the atmosphere, the hand-made day: no sunlight once the sun is well below the horizon, a redder sun low
    // down, a brighter one high up.
    void testDaylight() {
        LightLabSunSettings settings;
        settings.atmosphere = false;
        const Daylight night = daylightFor(directionTowardsSunOn(45.0f, 80.0f, 0.0f), settings);
        const Daylight evening = daylightFor(directionTowardsSunOn(45.0f, 80.0f, 17.5f), settings);
        const Daylight noon = daylightFor(directionTowardsSunOn(45.0f, 80.0f, 12.0f), settings);

        assert(night.sun.intensity == 0.0f);
        assert(night.sky.colour.y > 0.0f);
        assert(noon.sun.intensity == settings.sunIntensity);
        assert(evening.sun.intensity > 0.0f);
        assert(evening.sun.colour.z / evening.sun.colour.x < noon.sun.colour.z / noon.sun.colour.x);
        assert(noon.sky.colour.y > evening.sky.colour.y && evening.sky.colour.y > night.sky.colour.y);

        // The sky behind everything dims with it, and is blue: more blue than red.
        assert(noon.background.colour.y > evening.background.colour.y && evening.background.colour.y > night.background.colour.y);
        assert(noon.background.colour.z > noon.background.colour.x);

        // The flat ambient is the irradiance the drawn sky gives (a sky drawn at B lights a surface facing it with B), so
        // it matches the light probes, which see the drawn sky itself.
        for (const Daylight* daylight : {&night, &evening, &noon}) {
            assert(daylight->sky.colour.x == daylight->background.colour.x && daylight->sky.colour.y == daylight->background.colour.y &&
                   daylight->sky.colour.z == daylight->background.colour.z);
        }
    }

    /*
    * With the atmosphere (the default), the sun is its light above the air, white and the same at every hour: the
    * renderer dims and reddens it on its way down, and draws the sky and its disc, so the lab's own sky, ambient light
    * and disc are gone. Switching it off brings them back.
    */
    void testAtmosphere() {
        const LightLabSunSettings settings;
        assert(settings.atmosphere);
        const Daylight night = daylightFor(directionTowardsSunOn(45.0f, 80.0f, 0.0f), settings);
        const Daylight evening = daylightFor(directionTowardsSunOn(45.0f, 80.0f, 17.5f), settings);

        for (const Daylight* daylight : {&night, &evening}) {
            assert(daylight->sun.intensity == settings.sunIntensity);
            assert(daylight->sun.colour.x == daylight->sun.colour.z);
            assert(daylight->sky.colour.y == 0.0f && daylight->background.colour.y == 0.0f);
        }

        Lab lab;
        const FrameLighting lighting = collectLighting(lab.world, 1.0f);
        assert(lighting.atmosphere.has_value());
        assert(lighting.atmosphere->sunDiscScale == lab.game.getSunSettings().sunDiscScale);
        assert(lighting.atmosphere->groundAlbedo.y > lighting.atmosphere->groundAlbedo.x); // the planet beyond is grass too
        assert(!lab.world.get<ModelRenderer>(lab.game.getSunDisc()).visible);

        const Input noInput;
        lab.game.getSunSettings().atmosphere = false;
        lab.game.onUpdate(lab.world, noInput, 1.0f / 60.0f, 1.0f);
        assert(!collectLighting(lab.world, 1.0f).atmosphere.has_value());
        assert(lab.world.get<ModelRenderer>(lab.game.getSunDisc()).visible);

        lab.game.getSunSettings().atmosphere = true;
        lab.game.onUpdate(lab.world, noInput, 1.0f / 60.0f, 1.0f);
        assert(collectLighting(lab.world, 1.0f).atmosphere.has_value());
    }

    // The clock runs at its speed, wraps round midnight, and stands still at speed 0.
    void testClock() {
        assert(near(advanceSolarHours(10.0f, 0.5f, 4.0f), 12.0f, 1e-5f));
        assert(near(advanceSolarHours(23.5f, 1.0f, 1.0f), 0.5f, 1e-5f));
        assert(advanceSolarHours(15.5f, 0.0f, 100.0f) == 15.5f);

        Lab lab;
        lab.game.getSunSettings().hoursPerSecond = 1.0f;
        int ticks = 0;
        runTicks(lab.game, lab.world, static_cast<int>(std::lround(1.0f / tickSeconds)), ticks);
        assert(near(lab.game.getSunSettings().solarHours, 16.5f, 1e-3f));

        // The sun stands still by default, and golden views hold it at their hour even when it moves.
        assert(Lab{}.game.getSunSettings().hoursPerSecond == 0.0f);
        for (const GoldenView& view : Lab{}.game.goldenViews()) {
            Lab golden;
            golden.game.getSunSettings().hoursPerSecond = 1.0f;
            golden.game.prepareGoldenView(golden.world, view);
            const float preparedHours = golden.game.getSunSettings().solarHours;
            advanceToGoldenView(golden.game, golden.world, view, EngineSettings{}.ticksPerSecond);
            assert(golden.game.getSunSettings().solarHours == preparedHours);
        }
    }
    // The disc's direction from the camera and its angular radius as the camera sees it.
    float discAngularRadius(const Transform& pose, const Vec3& camera) {
        return std::asin(pose.scale.x / (pose.position - camera).length());
    }

    /*
    * A glowing disc in the sky where the sunlight comes from: along the sun's direction from the camera wherever it goes,
    * inside its far plane, at the sun's real angular size times the disc size setting, casting no shadow (it stands for
    * the light's own source), and glowing with the sunlight spread over its solid angle (capped). Gone at night. Only
    * without the atmosphere, which draws the disc itself.
    */
    void testSunDisc() {
        Lab lab;
        lab.game.getSunSettings().atmosphere = false;
        lab.game.onUpdate(lab.world, Input{}, 1.0f / 60.0f, 1.0f);
        const Entity disc = lab.game.getSunDisc();
        const DirectionalLight& sun = lab.world.get<DirectionalLight>(lab.game.getSun());

        const auto check = [&](float sizeTimes) {
            const Vec3 camera = lab.game.getCamera().getPosition();
            const Transform& pose = lab.world.get<Transform>(disc);
            const Vec3 offset = pose.position - camera;
            assert(near(offset.normalized().dot(sun.direction.normalized()), -1.0f, 1e-5f));
            assert(offset.length() < lab.game.getCamera().getFarPlane());
            assert(near(discAngularRadius(pose, camera), sun.angularRadius * sizeTimes, 1e-6f));

            const ModelRenderer& renderer = lab.world.get<ModelRenderer>(disc);
            const Vec3 expected = discRadiance(sun.colour * sun.intensity, sun.angularRadius * sizeTimes);
            const Vec3& glow = renderer.parts[0].material.emissive;
            assert(renderer.visible && !renderer.castsShadows);
            assert(near(glow.x, expected.x, 1e-3f) && near(glow.y, expected.y, 1e-3f) && near(glow.z, expected.z, 1e-3f));
        };

        check(1.0f);

        // It follows the camera, frame by frame.
        const Input noInput;
        lab.game.getCamera().setPosition(Vec3{120.0f, 30.0f, -260.0f});
        lab.game.onUpdate(lab.world, noInput, 1.0f / 60.0f, 1.0f);
        check(1.0f);

        // Larger for looks, and dimmer per pixel by as much, so it gives the same light.
        lab.game.getSunSettings().sunDiscScale = 2.0f;
        lab.game.onUpdate(lab.world, noInput, 1.0f / 60.0f, 1.0f);
        check(2.0f);

        // Midnight: the sun is below the ground, and so is its disc, hidden.
        lab.game.getSunSettings().solarHours = 0.0f;
        lab.game.onUpdate(lab.world, noInput, 1.0f / 60.0f, 1.0f);
        assert(!lab.world.get<ModelRenderer>(disc).visible);
    }
}

namespace {
    /*
    * The lab places no probes by hand: one ProbeCascades, following the camera, and the renderer keeps the probes round
    * it wherever it goes (render/gpu/probe_cascades.h).
    */
    void testTheLabsProbesFollowTheCamera() {
        Lab lab;
        const FrameLighting lighting = collectLighting(lab.world, 1.0f);
        assert(lighting.probeCascades.has_value());
        assert(lighting.probeCascades->followCamera);
    }
}

namespace {
    /*
    * The flashlight (gameplay/flashlight): off at the start, so the golden views keep the sunlit picture; F switches it
    * on and off; and every frame it is held in the right hand of the current camera, so it follows the view.
    */
    void testTheLabsFlashlight() {
        Lab lab;
        const Entity flashlight = lab.game.getFlashlight();
        assert(lab.world.isAlive(flashlight) && !lab.world.has<SpotLight>(flashlight));

        Input pressF;
        pressF.beginFrame();
        pressF.pressKey(Key::F);
        lab.game.onInput(lab.world, pressF);
        assert(lab.world.has<SpotLight>(flashlight));

        // Switching it is a scene event for the probes round it (gameplay/flashlight).
        assert(collectLighting(lab.world, 1.0f).probeEvents.size() == 1);
        clearProbeEvents(lab.world);

        Input idle;
        idle.beginFrame();
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        const FlashlightPose held = flashlightInHand(lab.game.getCamera());
        const Vec3 position = lab.world.get<Transform>(flashlight).position;
        assert(near(position.x, held.position.x, 1e-5f) && near(position.y, held.position.y, 1e-5f) && near(position.z, held.position.z, 1e-5f));
        assert(lab.world.get<SpotLight>(flashlight).direction.normalized().dot(held.direction.normalized()) > 0.99999f);

        lab.game.onInput(lab.world, pressF);
        assert(!lab.world.has<SpotLight>(flashlight));
    }

    // The probe events signalled since the last frame, cleared as the engine does once the frame is built.
    std::vector<ProbeEvent> takeProbeEvents(Lab& lab) {
        const std::vector<ProbeEvent> events = collectLighting(lab.world, 1.0f).probeEvents;
        clearProbeEvents(lab.world);
        return events;
    }

    /*
    * The back room's ceiling lamp: under the middle of the ceiling, off at the start (so the golden views keep their
    * picture), its fixture dark; L switches it on (a still light, not fast: its bounce goes into the probes' long memory)
    * and the fixture glows, and each switch signals a light change to the probes over the lamp's reach.
    */
    void testTheBackRoomLamp() {
        Lab lab;
        const Entity lamp = lab.game.getLamp();
        assert(lab.world.isAlive(lamp) && !lab.world.has<PointLight>(lamp));
        const Vec3 position = lab.world.get<Transform>(lamp).position;
        assert(labBackRoom.contains(position) && position.y > labRoomHeight - 0.3f);
        assert(!lab.world.get<ModelRenderer>(lamp).castsShadows);
        assert(lab.world.get<ModelRenderer>(lamp).parts[0].material.emissive.x == 0.0f);
        assert(takeProbeEvents(lab).empty());

        Input pressL;
        pressL.beginFrame();
        pressL.pressKey(Key::L);

        for (bool on : {true, false}) {
            lab.game.onInput(lab.world, pressL);
            assert(lab.world.has<PointLight>(lamp) == on);
            assert((lab.world.get<ModelRenderer>(lamp).parts[0].material.emissive.x > 0.0f) == on);

            const std::vector<ProbeEvent> events = takeProbeEvents(lab);
            assert(events.size() == 1);
            assert(events[0].kind == ProbeEventKind::LightChange && !events[0].everywhere);
            const float reach = labLampLight.range;
            assert(events[0].region.contains(position + Vec3{reach, reach, reach} * 0.99f));
            assert(events[0].region.contains(position - Vec3{reach, reach, reach} * 0.99f));
        }

        assert(!labLampLight.fast && labLampLight.castsShadows);
    }

    /*
    * Moving the clock by hand (the solar time's slider) jumps the sun and sky: a light change everywhere, once the jump
    * passes labClockJumpHours, small drags adding up. The clock running by itself moves the sun smoothly and signals
    * nothing.
    */
    void testAClockJumpRestartsTheProbes() {
        Lab lab;
        Input idle;
        idle.beginFrame();
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        assert(takeProbeEvents(lab).empty());

        LightLabSunSettings& sun = lab.game.getSunSettings();
        sun.solarHours += 0.6f * labClockJumpHours;
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        assert(takeProbeEvents(lab).empty());

        sun.solarHours += 0.6f * labClockJumpHours;
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        const std::vector<ProbeEvent> events = takeProbeEvents(lab);
        assert(events.size() == 1 && events[0].kind == ProbeEventKind::LightChange && events[0].everywhere);

        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        assert(takeProbeEvents(lab).empty());

        // Across midnight the jump is the short way round.
        sun.solarHours = 23.99f;
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        takeProbeEvents(lab);
        sun.solarHours = 0.01f;
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        assert(takeProbeEvents(lab).empty());

        sun.hoursPerSecond = 1.0f;
        int ticks = 0;
        runTicks(lab.game, lab.world, static_cast<int>(std::lround(1.0f / tickSeconds)), ticks);
        lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
        assert(takeProbeEvents(lab).empty());
    }
}

void testLightLab() {
    testTheLevelIsStatic();
    testTheLabsFlashlight();
    testTheBackRoomLamp();
    testAClockJumpRestartsTheProbes();
    testTheLabsProbesFollowTheCamera();
    testSunDisc();
    testTowersDoubleAndFanOut();
    testFence();
    testForest();
    testBuildingOpenings();
    testProbesLeaveTheWindowWall();
    testTheBackRoomsCoarseProbeKeepsAwayFromTheDoor();
    testSunbeamThroughTheWindow();
    testWalkingIn();
    testDaylight();
    testAtmosphere();
    testClock();
}
