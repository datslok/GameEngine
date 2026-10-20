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
#include "scene/lighting.h"
#include "scene/model_renderer.h"
#include "scene/sky_object.h"
#include "scene/sun.h"

#include "demo_helpers.h"
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

        // The sun component follows the settings, and the sky shows behind everything.
        assert(collectLighting(lab.world, 1.0f).background.y > 0.0f);
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

    // Day and night: no sunlight once the sun is well below the horizon, a redder sun low down, a brighter one high up.
    void testDaylight() {
        const LightLabSunSettings settings;
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
    * the light's own source), and glowing with the sunlight spread over its solid angle (capped). Gone at night.
    */
    void testSunDisc() {
        Lab lab;
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
    bool inside(const Aabb& box, const Vec3& point) {
        return point.x > box.minimum.x && point.x < box.maximum.x && point.y > box.minimum.y && point.y < box.maximum.y &&
               point.z > box.minimum.z && point.z < box.maximum.z;
    }

    /*
    * The light probes round the building: none buried in a wall, the floor or the roof (until probes move themselves
    * out, a buried probe only sees the inside of the wall), each room holding at least one full cell of 8 (two probes
    * along every axis inside it), and the lab hands the renderer this volume.
    */
    void testProbeVolume() {
        validateProbeVolume(labProbeVolume);
        const LightLabLayout layout = buildLightLabLayout();
        const int probes = probeCount(labProbeVolume.counts);

        for (int probe = 0; probe < probes; ++probe) {
            const Vec3 position = probePosition(labProbeVolume, probe);

            for (const LabBlock& block : layout.building) {
                assert(!inside(block.bounds(), position));
            }
        }

        for (const Aabb& room : {labSunlitRoom, labBackRoom}) {
            int insideAlong[3] = {0, 0, 0};

            for (int x = 0; x < labProbeVolume.counts.x; ++x) {
                const float at = labProbeVolume.origin.x + labProbeVolume.spacing.x * static_cast<float>(x);
                insideAlong[0] += at > room.minimum.x && at < room.maximum.x ? 1 : 0;
            }

            for (int y = 0; y < labProbeVolume.counts.y; ++y) {
                const float at = labProbeVolume.origin.y + labProbeVolume.spacing.y * static_cast<float>(y);
                insideAlong[1] += at > room.minimum.y && at < room.maximum.y ? 1 : 0;
            }

            for (int z = 0; z < labProbeVolume.counts.z; ++z) {
                const float at = labProbeVolume.origin.z + labProbeVolume.spacing.z * static_cast<float>(z);
                insideAlong[2] += at > room.minimum.z && at < room.maximum.z ? 1 : 0;
            }

            assert(insideAlong[0] >= 2 && insideAlong[1] >= 2 && insideAlong[2] >= 2);
        }

        Lab lab;
        const FrameLighting lighting = collectLighting(lab.world, 1.0f);
        assert(lighting.probeVolumes.size() == 1);
        assert(lighting.probeVolumes[0].counts.x == labProbeVolume.counts.x);
        assert(lighting.probeVolumes[0].origin.z == labProbeVolume.origin.z);
    }

    /*
    * The dense probes, a test of finer bounce light: 20 cm apart, inside the building only. They must stay within what
    * the renderer can hold (validateProbeVolume throws otherwise), and light every corner of both rooms at the volume's
    * full weight, so no part of a room falls back on the flat ambient.
    */
    void testDenseProbes() {
        validateProbeVolume(labDenseProbeVolume);
        assert(labDenseProbeVolume.spacing.x == 0.2f && labDenseProbeVolume.spacing.y == 0.2f && labDenseProbeVolume.spacing.z == 0.2f);
        assert(probeCount(labDenseProbeVolume.counts) <= maxProbesPerVolume);

        for (const Aabb& room : {labSunlitRoom, labBackRoom}) {
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 point{(corner & 1) != 0 ? room.maximum.x : room.minimum.x, (corner & 2) != 0 ? room.maximum.y : room.minimum.y,
                                 (corner & 4) != 0 ? room.maximum.z : room.minimum.z};
                assert(probeVolumeWeight(labDenseProbeVolume, point) > 0.999f);
            }
        }

        // Off by default; switched on, the lab's volume becomes the dense one at the next frame, and back again.
        Lab lab;
        assert(!lab.game.getProbeSettings().denseProbes);

        const auto volumeNow = [&] {
            Input idle;
            idle.beginFrame();
            lab.game.onUpdate(lab.world, idle, 1.0f / 60.0f, 1.0f);
            const FrameLighting lighting = collectLighting(lab.world, 1.0f);
            assert(lighting.probeVolumes.size() == 1);
            return lighting.probeVolumes[0];
        };

        assert(volumeNow().counts.x == labProbeVolume.counts.x);
        lab.game.getProbeSettings().denseProbes = true;
        const ProbeVolume dense = volumeNow();
        assert(dense.counts.x == labDenseProbeVolume.counts.x && dense.counts.z == labDenseProbeVolume.counts.z);
        assert(dense.spacing.y == labDenseProbeVolume.spacing.y && dense.origin.x == labDenseProbeVolume.origin.x);
        lab.game.getProbeSettings().denseProbes = false;
        assert(volumeNow().spacing.x == labProbeVolume.spacing.x);
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
    testProbeVolume();
    testDenseProbes();
    testSunDisc();
    testTowersDoubleAndFanOut();
    testFence();
    testForest();
    testBuildingOpenings();
    testSunbeamThroughTheWindow();
    testWalkingIn();
    testDaylight();
    testClock();
}
