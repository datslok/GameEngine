#include "games/demo/demo_game.h"
#include "gameplay/character.h"
#include "gameplay/firefly.h"
#include "gameplay/first_person_body.h"
#include "gameplay/spinner.h"
#include "math/angles.h"
#include "math/transform.h"
#include "engine/debug_views.h"
#include "physics/collider_raycast.h"
#include "scene/debug_text.h"
#include "scene/camera_ray.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/model_renderer.h"

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
    // The night's light: a faint grey ambient and a weak, cool moon (linear units).
    constexpr float nightAmbient = 0.012f;
    constexpr float moonlightIntensity = 0.06f;

    // How brightly the moon sphere's surface glows (times its tint). Only the object: the moonlight above is separate.
    // Real light sources are far brighter than what they light, and well past white the tone mapping curve turns them
    // white-hot while the bloom's few percent of scattered light becomes a visible halo.
    constexpr float moonGlowBrightness = 6.0f;

    // Where the duck holds the flashlight in MOBA mode, and how far it tilts the beam down (radians, about 20 degrees).
    constexpr float flashlightHeight = 1.2f;
    constexpr float flashlightTilt = 0.35f;

    // How far in front of the duck's centre the flashlight is held: past its body (the model is 2 units long), so the duck
    // does not block its own light now that every light casts shadows.
    constexpr float flashlightReach = 1.2f;

    // Away from MOBA mode the flashlight is in the right hand: this far right of and below the eye, aimed at the point this
    // far straight ahead. Being off to the side, it throws shadows you can see beside things, not hidden behind them.
    constexpr float flashlightHandRight = 0.4f;
    constexpr float flashlightHandDown = 0.3f;
    constexpr float flashlightAimDistance = 8.0f;

    // Which way the moon is from where the free camera starts: up, to the left and behind. The moonlight travels the opposite way.
    const Vec3 towardsMoon{-0.4f, 1.0f, 0.6f};

    // The moon is a place you can fly to: this far from where the free camera starts (inside the camera's far plane of 100).
    // At the free camera's 3 units per second, reaching its surface takes about 20 seconds.
    const Vec3 freeCameraStart{2.0f, 1.0f, 0.0f};
    constexpr float moonDistance = 64.0f;
    constexpr float moonRadius = 4.0f;

    // The outline of the selected object (linear cyan), distinct from the engine's orange collider view.
    const Vec3 selectionColour{0.1f, 0.9f, 1.0f};

    // The outline of whatever the cursor is over: the selection's cyan, dimmed.
    const Vec3 hoverColour = selectionColour * 0.35f;

    // How long the face normal drawn on a hovered level triangle is, in world units.
    constexpr float triangleNormalLength = 0.5f;

    // How far above the clicked surface the walk target marker floats, so the surface does not hide it.
    constexpr float markerLift = 0.02f;

    // A MOBA unit's ground acceleration: high enough to reach full speed (and stop) within a tick.
    constexpr float mobaUnitAcceleration = 1000.0f;

    // The lamp field (L): a grid of 10 x 11 places, 3.5 units apart, covering the ground up to the obstacle course, minus
    // the 10 places in a keep-clear box around the spinning objects, the duck and the first-person start: 100 lamps, well
    // past the shader's 64 point light seats, so light priority has to choose (and fade seats and shadows between lamps).
    constexpr int lampColumns = 10;
    constexpr int lampRows = 11;
    constexpr float lampSpacing = 3.5f;
    const Vec3 firstLampBase{-19.25f, 0.0f, -24.25f};
    const Vec2 lampKeepClearMinimum{-4.5f, -14.5f}; // x and z on the ground
    const Vec2 lampKeepClearMaximum{4.5f, 1.5f};

    // Lanterns floating at about waist height, bright enough to light a pool a few units across.
    constexpr float lampIntensity = 1.5f;
    constexpr float lampRange = 5.0f;
    constexpr float lampSourceRadius = 0.5f;
    constexpr float bulbRadius = 0.12f;

    // A pillar beside each lamp, taller than the lamp, so a shadowed lamp throws a long streak away from it.
    constexpr float pillarDistance = 0.8f;
    constexpr float pillarHalfHeight = 0.7f;
    constexpr float pillarHalfWidth = 0.15f;

    // Each pillar is turned a golden angle (about 137.5 degrees) further than the last, so the shadows point every which way.
    constexpr float goldenAngle = 2.39996f;

    // Each lamp flies round its pillar like a firefly, so the pillar's shadow sweeps round like a sundial's. It keeps at
    // least 0.55 from the pillar's middle (the pillar's corners are 0.21 out, the bulb 0.12 across) and stays below its
    // top (1.4), so the shadow is always a long streak. Speeds differ from lamp to lamp and alternate direction.
    constexpr float lampOrbitRadius = 0.75f;
    constexpr float lampOrbitWobble = 0.2f;
    constexpr float lampHeight = 1.0f;
    constexpr float lampHeightWobble = 0.35f;
    constexpr float lampSlowestOrbit = 0.35f;
    constexpr float lampOrbitSpread = 0.4f;

    // The obstacle course, in its own coordinates around obstacleCourseOrigin: a 6 x 16 floor slab with a wall and doorway
    // across it, five 0.25-high steps up to a platform, a gentle ramp (20 degrees, walkable) and a steep one (50 degrees,
    // too steep to walk up), all for the character controller to try.
    constexpr float courseHalfWidth = 3.0f;
    constexpr float courseHalfLength = 8.0f;
    constexpr float wallHeight = 2.5f;
    constexpr float wallThickness = 0.3f;
    constexpr float doorwayHalfWidth = 0.75f;
    constexpr float stairRun = 0.5f;
    constexpr float gentleSlopeDegrees = 20.0f;
    constexpr float steepSlopeDegrees = 50.0f;
    constexpr float rampThickness = 0.2f;

    // Floodlights: four masts standing just inside the course's corners, each lamp aimed down across the middle so the
    // beams cross and fill in each other's shadows, the way a stadium lights its pitch. Brightness falls off as
    // intensity / (d^2 + r^2): the course is 8 to 14 away from a lamp, which this intensity lights at about 0.3 to 0.5.
    constexpr float floodlightPoleInset = 0.3f;       // from the slab's edges
    constexpr float floodlightPoleHalfWidth = 0.15f;
    constexpr float floodlightHeadOffset = 0.5f;      // the lamp sits this far out from the pole, towards the course
    constexpr float floodlightAimAcross = 0.3f;       // aims at the point this fraction of the way past the middle
    constexpr float floodlightIntensity = 40.0f;
    constexpr float floodlightRange = 25.0f;
    constexpr float floodlightInnerDegrees = 20.0f;
    constexpr float floodlightOuterDegrees = 35.0f;
    constexpr float floodlightSourceRadius = 1.5f;
    constexpr float floodlightEmitterRadius = 0.15f;  // a big glowing face, so soft-edged shadows
    const Vec3 floodlightColour{1.0f, 0.97f, 0.92f};  // a cool white, unlike the warm flashlight
    constexpr float floodlightGlowBrightness = 20.0f; // the lamp heads' glowing surface (times the colour), not the light
    const Vec3 floodlightHeadHalfSize{0.4f, 0.2f, 0.4f};

    /*
    * One block of the course: the unit cube (-1..1) stretched to the half-sizes, turned, then moved to its centre.
    */
    RenderPart courseBlock(MeshHandle cube, const Material& material, const Vec3& centre, const Vec3& halfSize,
                           const Mat4& turn = Mat4::identity()) {
        return RenderPart{cube, material, Mat4::translation(centre.x, centre.y, centre.z) * turn * Mat4::scaling(halfSize.x, halfSize.y, halfSize.z)};
    }

    /*
    * A ramp rising towards +z: a thin block tipped up by the slope angle about x (a negative turn about x lifts the +z end),
    * placed so its low end rests on the floor slab at lowZ.
    */
    RenderPart courseRamp(MeshHandle cube, const Material& material, float centreX, float lowZ, float length, float slopeDegrees) {
        const float slope = radiansFromDegrees(slopeDegrees);
        const float halfLength = length * 0.5f;
        const Vec3 centre{centreX, obstacleCourseFloorTop + halfLength * std::sin(slope), lowZ + halfLength * std::cos(slope)};
        return courseBlock(cube, material, centre, Vec3{1.0f, rampThickness * 0.5f, halfLength}, Mat4::rotationX(-slope));
    }

    /*
    * Every block of the course as parts of one renderer, so it is a single entity; the level's collision mesh is made from
    * the same parts, so what is drawn and what is solid cannot disagree.
    */
    ModelRenderer buildObstacleCourse(MeshHandle cube, const Material& stone) {
        ModelRenderer course;
        const float floorHalfHeight = obstacleCourseFloorTop * 0.5f;
        course.parts.push_back(courseBlock(cube, stone, Vec3{0.0f, floorHalfHeight, 0.0f}, Vec3{courseHalfWidth, floorHalfHeight, courseHalfLength}));

        // A wall across the slab at z = -2, with a doorway in the middle.
        const float wallPieceHalf = (courseHalfWidth - doorwayHalfWidth) * 0.5f;
        const float wallCentreY = obstacleCourseFloorTop + wallHeight * 0.5f;
        for (const float side : {-1.0f, 1.0f}) {
            course.parts.push_back(courseBlock(cube, stone, Vec3{side * (doorwayHalfWidth + wallPieceHalf), wallCentreY, -2.0f},
                                               Vec3{wallPieceHalf, wallHeight * 0.5f, wallThickness * 0.5f}));
        }

        // Stairs on the -x half, rising towards +z from z = 1; each step is a solid block from the slab up to its tread.
        const float stairsEnd = 1.0f + stairRun * static_cast<float>(obstacleCourseStairSteps);
        for (int step = 0; step < obstacleCourseStairSteps; ++step) {
            const float start = 1.0f + stairRun * static_cast<float>(step);
            const float top = obstacleCourseFloorTop + obstacleCourseStairRise * static_cast<float>(step + 1);
            course.parts.push_back(courseBlock(cube, stone, Vec3{-2.0f, (obstacleCourseFloorTop + top) * 0.5f, (start + stairsEnd + 2.5f) * 0.5f},
                                               Vec3{1.0f, (top - obstacleCourseFloorTop) * 0.5f, (stairsEnd + 2.5f - start) * 0.5f}));
        }

        // The ramps on the +x half: the gentle one rising beside the stairs, the steep one beyond the wall.
        course.parts.push_back(courseRamp(cube, stone, 1.5f, 1.0f, 4.0f, gentleSlopeDegrees));
        course.parts.push_back(courseRamp(cube, stone, 1.5f, -7.0f, 3.0f, steepSlopeDegrees));

        // The floodlight poles, from the slab up to the lamps. They are part of the course, so they are solid and stay when
        // the lamps are switched off.
        const float poleHalfHeight = (floodlightHeight - obstacleCourseFloorTop) * 0.5f;
        for (const Vec3& foot : floodlightPoleFeet()) {
            const Vec3 local = foot - obstacleCourseOrigin;
            course.parts.push_back(courseBlock(cube, stone, Vec3{local.x, obstacleCourseFloorTop + poleHalfHeight, local.z},
                                               Vec3{floodlightPoleHalfWidth, poleHalfHeight, floodlightPoleHalfWidth}));
        }
        return course;
    }
}

DemoGame::DemoGame(ControlMode startMode, bool debugModeSwitching):
    camera(
        Vec3{2.0f, 1.0f, 0.0f},
        Vec3{0.0f, 0.0f, -5.0f},
        Vec3{0.0f, 1.0f, 0.0f},
        radiansFromDegrees(70.0f), // FoV
        16.0f / 9.0f, // Replaced by the engine with the real frame's aspect ratio.
        0.1f,
        100.0f
    ),
    // The camera controllers start with their default settings; change them through getSettings().
    controlMode(startMode),
    enableDebugModeSwitching(debugModeSwitching)
{
}

void DemoGame::onInit(World& world, AssetManager& assets) {
    createScene(world, assets);

    // Start from the mode's predictable camera pose.
    setControlMode(world, controlMode);
}

void DemoGame::createScene(World& world, AssetManager& assets) {
    // Each file is loaded once; both cubes share one mesh handle and one texture handle (the lamps reuse the cube and sphere).
    cubeMesh = assets.addMesh(Mesh::cube());
    sphereMesh = assets.addMesh(Mesh::sphere());
    const MeshHandle pyramidMesh = assets.loadMesh("assets/models/pyramid.obj");
    // The teapot file has no normals; it is a curved surface, so smooth them. The pyramid's flat faces are meant to look flat.
    const MeshHandle teapotMesh = assets.loadMesh("assets/models/teapot.obj", MeshLoadOptions{.smoothNormals = true});

    // A collider for a prop: the box around its mesh for quick tests, and the triangles themselves for exact clicks.
    const auto propCollider = [&assets](MeshHandle mesh) {
        return meshCollider(std::make_shared<const TriangleMesh>(assets.makeCollisionMesh(makeMeshRenderer(mesh))));
    };
    cubeCollider = propCollider(cubeMesh);

    // A spinning demo object: Transform + ModelRenderer + Spinner + Collider, plus PreviousTransform so it is drawn smoothly.
    const auto spawnSpinner = [&world](MeshHandle mesh, const Material& material, const Collider& collider,
                                       const Transform& transform, const Vec3& speed) {
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, PreviousTransform{transform});
        world.add(entity, makeMeshRenderer(mesh, material));
        world.add(entity, Spinner{transform.rotation, speed});
        world.add(entity, collider);
    };

    Material textured;
    textured.texture = assets.loadTexture("assets/textures/demo.png");

    Material green;
    green.colour = Pixel{80, 200, 120};

    Material gold;
    gold.colour = Pixel{230, 180, 60};

    Material grenadeShell;
    grenadeShell.colour = Pixel{70, 80, 50};
    grenadeLooks = makeMeshRenderer(sphereMesh, grenadeShell);

    const Vec3 smallScale{0.7f, 0.7f, 0.7f};

    Transform first;
    first.position = Vec3{-3.0f, 0.0f, -10.0f};
    first.rotation.x = 0.3f;
    spawnSpinner(cubeMesh, textured, cubeCollider, first, Vec3{2.0f, 2.0f, 0.0f});

    Transform second;
    second.position = Vec3{3.0f, 0.0f, -10.0f};
    second.scale = smallScale;
    spawnSpinner(cubeMesh, textured, cubeCollider, second, Vec3{0.0f, -1.0f, 0.0f});

    Transform third;
    third.position = Vec3{0.0f, 3.0f, -10.0f};
    third.scale = smallScale;
    spawnSpinner(pyramidMesh, green, propCollider(pyramidMesh), third, Vec3{0.0f, 1.0f, 0.0f});

    Transform fourth;
    // The teapot's origin is at its base, so y = 0 stands it on the grass. Set back so its spout clears the cubes as it spins.
    fourth.position = Vec3{0.0f, 0.0f, -13.0f};
    fourth.scale = smallScale;
    spawnSpinner(teapotMesh, gold, propCollider(teapotMesh), fourth, Vec3{0.0f, -1.0f, 0.0f});

    // Night: a faint ambient light and a weak, cool moon, so the flashlight stands out.
    // Unlike ambient light, the moon comes from a direction, so shapes keep a lit side and a dark side outside the beam, and cast shadows.
    // It is a directional light, like the real moon: parallel rays, the same brightness everywhere.
    // Light values are linear: the screen's sRGB encoding brightens dark values a lot, so night light is tiny in these units.
    world.add(world.create(), AmbientLight{Vec3{nightAmbient, nightAmbient, nightAmbient}});
    world.add(world.create(), DirectionalLight{towardsMoon * -1.0f, Vec3{0.32f, 0.45f, 1.0f}, moonlightIntensity});

    // The moon itself: a glowing sphere in the direction the moonlight comes from, at a fixed place so you can fly to it.
    Material moonGlow;
    moonGlow.colour = Pixel{0, 0, 0};
    moonGlow.specularStrength = 0.0f;
    moonGlow.emissive = Vec3{0.69f, 0.79f, 1.0f} * moonGlowBrightness;

    Transform moonPlacement;
    moonPlacement.scale = Vec3{moonRadius, moonRadius, moonRadius};
    moonPlacement.position = freeCameraStart + towardsMoon.normalized() * moonDistance;

    moon = world.create();
    world.add(moon, moonPlacement);
    // It stands for the moonlight's source, so it must not block that light (its shadow box reaches back towards the moon).
    ModelRenderer moonRenderer = makeMeshRenderer(sphereMesh, moonGlow);
    moonRenderer.castsShadows = false;
    world.add(moon, std::move(moonRenderer));
    // The sphere mesh has radius 1, so a unit sphere collider scaled with it fits exactly; it needs no triangles.
    world.add(moon, Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});

    // The flashlight. updateFlashlight places it every frame, at the camera or (in MOBA mode) in the duck's hands.
    flashlight = world.create();
    world.add(flashlight, Transform{});
    world.add(flashlight, flashlightBeam);

    // Ground: it never moves, so it needs no PreviousTransform.
    Material grass;
    grass.colour = Pixel{75, 110, 75};
    grass.specularStrength = 0.0f; // Grass is matte.

    Transform groundPlacement;
    groundPlacement.position = Vec3{0.0f, 0.0f, -6.0f};

    const Entity ground = world.create();
    world.add(ground, groundPlacement);
    world.add(ground, makeMeshRenderer(assets.addMesh(Mesh::plane(20.0f)), grass));

    // The player's own body for first person: an invisible capsule standing where the first-person view starts.
    firstPersonBody = spawnFirstPersonBody(world, Vec3{0.0f, 0.0f, 0.0f});

    // The obstacle course: plain stone blocks for the character controller to walk on, climb and bump into.
    Material courseStone;
    courseStone.colour = Pixel{150, 148, 140};
    courseStone.specularStrength = 0.1f;

    Transform coursePlacement;
    coursePlacement.position = obstacleCourseOrigin;
    ModelRenderer course = buildObstacleCourse(cubeMesh, courseStone);

    // The static level: the ground and the course never move, so their triangles go into the level once, made from the
    // same parts that are drawn.
    staticLevel.add(assets.makeCollisionMesh(world.get<ModelRenderer>(ground)), groundPlacement.getMatrix());
    staticLevel.add(assets.makeCollisionMesh(course), coursePlacement.getMatrix());

    const Entity courseEntity = world.create();
    world.add(courseEntity, coursePlacement);
    world.add(courseEntity, std::move(course));

    // The floodlights over the course start switched on.
    toggleFloodlights(world);

    // Change this configuration to use another compatible static model. Size and speeds keep CharacterConfig's defaults.
    const CharacterConfig playerConfig{
        .modelPath = "assets/models/Duck.glb",
        .modelForwardYaw = radiansFromDegrees(90.0f),
        // A MOBA unit starts and stops at once, so it arrives exactly where it was sent; a little wider than a hero.
        .controller = CharacterControllerSettings{.radius = 0.5f, .groundAcceleration = mobaUnitAcceleration}
    };

    const Model& model = assets.loadModel(playerConfig.modelPath);
    player = spawnCharacter(world, assets, model, playerConfig, Vec3{0.0f, 0.0f, -6.0f});

    // Movement marker. It jumps to each click instead of gliding, so it has no PreviousTransform.
    Material yellow;
    // It glows instead of reflecting light, so it is the same yellow day or night. It will become a HUD ring in phase 9.
    yellow.colour = Pixel{0, 0, 0};
    yellow.specularStrength = 0.0f;
    yellow.emissive = Vec3{1.0f, 0.71f, 0.022f};

    ModelRenderer markerRenderer = makeMeshRenderer(assets.addMesh(Mesh::plane(0.2f)), yellow);
    markerRenderer.visible = false;

    // Slightly above the ground to avoid overlapping surfaces.
    Transform markerPlacement;
    markerPlacement.position = Vec3{0.0f, markerLift, 0.0f};

    destinationMarker = world.create();
    world.add(destinationMarker, markerPlacement);
    world.add(destinationMarker, std::move(markerRenderer));
}

void DemoGame::onInput(World& world, const Input& input) {
    // Pick using the camera pose before this frame's panning.
    updatePlayerCommands(world, input);
    updateFirstPersonCommands(world, input);
    updateSelection(world, input);

    if (input.wasKeyPressed(Key::F)) {
        toggleFlashlight(world);
    }

    if (input.wasKeyPressed(Key::L)) {
        toggleLamps(world);
    }

    if (input.wasKeyPressed(Key::K)) {
        toggleFloodlights(world);
    }

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }
}

bool DemoGame::wantsDebugView() const {
    return debugViewEnabled;
}

/*
* Lights are invisible except for what they light, so each gets a small sphere in its own colour, and a spotlight a line
* along its beam. The walk target is the destination marker, which is shown exactly while the duck is walking to it.
*/
void DemoGame::onDebugDraw(const World& world, DebugDraw& debug, float alpha) {
    // The selection and the hovered object are outlined whether or not the debug view is on: they stand in for selection
    // rings until the HUD exists. Something both hovered and selected keeps the brighter selection outline.
    const auto outline = [&](Entity entity, const Vec3& colour) {
        if (!world.isAlive(entity)) {
            return;
        }
        if (const Collider* collider = world.tryGet<Collider>(entity)) {
            const Mat4 model = getRenderTransform(world, entity, alpha).getMatrix();
            debug.shape(worldShape(collider->shape, model), colour);
        }
    };
    outline(selected, selectionColour);
    if (!(hovered == selected)) {
        outline(hovered, hoverColour);
    }

    if (!debugViewEnabled) {
        return;
    }

    // Below the engine's lines: what each system costs per tick (averages; the engine's TICK line has the worst whole tick).
    drawProfile(systemTimes, firstGameReadoutLine, 3, false, profileDebugColour, debug);

    // How many colliders there are, and how few the hover ray had to test.
    const std::string broadPhase = "COLLIDERS " + std::to_string(collisionScene.getColliders().size()) +
                                   "  TESTED " + std::to_string(hoverStats.tested) +
                                   "  CELLS " + std::to_string(hoverStats.cellsWalked);
    debugText(debug, debugReadoutLine(firstGameReadoutLine + 1), debugReadoutTextHeight, broadPhase, colliderDebugColour);

    // The level triangle under the cursor, with a short line along its face normal, and what the level query cost.
    if (hoveredTriangle) {
        const auto corners = staticLevel.getTriangle(*hoveredTriangle);
        debug.line(corners[0], corners[1], hoverColour);
        debug.line(corners[1], corners[2], hoverColour);
        debug.line(corners[2], corners[0], hoverColour);
        const Vec3 middle = (corners[0] + corners[1] + corners[2]) / 3.0f;
        const Vec3 normal = (corners[1] - corners[0]).cross(corners[2] - corners[0]).normalized();
        debug.line(middle, middle + normal * triangleNormalLength, hoverColour);
    }
    const std::string levelLine = "LEVEL TRIANGLES " + std::to_string(staticLevel.getTriangleCount()) +
                                  "  TESTED " + std::to_string(levelHoverStats.tested) +
                                  "  CELLS " + std::to_string(levelHoverStats.cellsWalked);
    debugText(debug, debugReadoutLine(firstGameReadoutLine + 2), debugReadoutTextHeight, levelLine, colliderDebugColour);

    // The duck's character controller: what it stands on (a line along the ground normal at its feet), how steep that is,
    // and how many substeps its last move took.
    if (world.isAlive(player)) {
        const CharacterController& controller = world.get<CharacterController>(player);
        const Vec3 feet = getRenderTransform(world, player, alpha).position;
        if (controller.grounded) {
            debug.line(feet, feet + controller.groundNormal * triangleNormalLength, colliderDebugColour);
        }
        const int slopeDegrees = static_cast<int>(std::lround(degreesFromRadians(std::acos(std::clamp(controller.groundNormal.y, -1.0f, 1.0f)))));
        const std::string duckLine = std::string("DUCK ") + (controller.grounded ? "GROUNDED" : "AIRBORNE") +
                                     "  SLOPE " + std::to_string(slopeDegrees) +
                                     "  SUBSTEPS " + std::to_string(controller.lastSubsteps);
        debugText(debug, debugReadoutLine(firstGameReadoutLine + 3), debugReadoutTextHeight, duckLine, colliderDebugColour);
    }

    constexpr float markerRadius = 0.25f;
    constexpr float beamLength = 2.0f;

    world.each<PointLight, Transform>([&](Entity entity, const PointLight& light, const Transform&) {
        debug.sphere(getRenderTransform(world, entity, alpha).position, markerRadius, light.colour);
    });

    world.each<SpotLight, Transform>([&](Entity entity, const SpotLight& light, const Transform&) {
        const Vec3 position = getRenderTransform(world, entity, alpha).position;
        debug.sphere(position, markerRadius, light.colour);
        debug.line(position, position + light.direction.normalized() * beamLength, light.colour);
    });

    if (world.isAlive(player) && world.get<ModelRenderer>(destinationMarker).visible) {
        const Vec3 from = getRenderTransform(world, player, alpha).position;
        debug.line(from, world.get<Transform>(destinationMarker).position, Vec3{1.0f, 0.71f, 0.022f});
    }
}

/*
* Switching off removes the light rather than dimming it to zero, so the renderer does not spend a slot on it.
*/
void DemoGame::toggleFlashlight(World& world) {
    if (world.has<SpotLight>(flashlight)) {
        world.remove<SpotLight>(flashlight);
    }
    else {
        world.add(flashlight, flashlightBeam);
    }
}

/*
* The lamps are whole entities, created and destroyed together, so off means truly gone: no seats, no draws.
* Four colours in turn make it easy to see which lamps are lit: an unseated lamp's bulb still glows (it is emissive),
* but the ground around it stays dark. Each lamp flies round its pillar (a Firefly), so it has a PreviousTransform and is
* placed where its flight puts it right now, or the first tick would make it jump. The pillars never move.
*/
void DemoGame::toggleLamps(World& world) {
    if (!lampEntities.empty()) {
        for (const Entity entity : lampEntities) {
            world.destroy(entity);
        }

        lampEntities.clear();
        return;
    }

    const Vec3 colours[] = {
        Vec3{1.0f, 0.55f, 0.2f},  // warm orange
        Vec3{0.25f, 0.85f, 1.0f}, // cyan
        Vec3{1.0f, 0.3f, 0.7f},   // pink
        Vec3{0.55f, 1.0f, 0.3f}   // lime
    };

    Material stone;
    stone.colour = Pixel{120, 120, 130};
    stone.specularStrength = 0.05f;

    int index = 0;

    for (int row = 0; row < lampRows; ++row) {
        for (int column = 0; column < lampColumns; ++column) {
            const Vec3 base = firstLampBase + Vec3{lampSpacing * static_cast<float>(column), 0.0f, lampSpacing * static_cast<float>(row)};
            const bool inKeepClear = base.x >= lampKeepClearMinimum.x && base.x <= lampKeepClearMaximum.x &&
                                     base.z >= lampKeepClearMinimum.y && base.z <= lampKeepClearMaximum.y;
            if (inKeepClear) {
                continue;
            }
            const Vec3& colour = colours[index % 4];

            // The bulb glows in the light's colour and must not block the light inside it.
            Material glow;
            glow.colour = Pixel{0, 0, 0};
            glow.specularStrength = 0.0f;
            glow.emissive = colour;

            ModelRenderer bulb = makeMeshRenderer(sphereMesh, glow);
            bulb.castsShadows = false;

            const float angle = goldenAngle * static_cast<float>(index);
            const Vec3 pillarFoot = base + Vec3{std::cos(angle) * pillarDistance, 0.0f, std::sin(angle) * pillarDistance};

            // The golden angle again, for the starting point and the wobbles, so neighbours fly out of step.
            const float speed = lampSlowestOrbit + lampOrbitSpread * static_cast<float>(index % 5) / 4.0f;
            const Firefly flight{
                .centre = pillarFoot,
                .orbitRadius = lampOrbitRadius,
                .radiusWobble = lampOrbitWobble,
                .height = lampHeight,
                .heightWobble = lampHeightWobble,
                .orbitSpeed = index % 2 == 0 ? speed : -speed,
                .phase = angle
            };

            Transform lampPlacement;
            lampPlacement.position = fireflyPosition(flight, latestSimulationSeconds);
            lampPlacement.scale = Vec3{bulbRadius, bulbRadius, bulbRadius};

            const Entity lamp = world.create();
            world.add(lamp, lampPlacement);
            world.add(lamp, PreviousTransform{lampPlacement});
            world.add(lamp, flight);
            world.add(lamp, std::move(bulb));
            world.add(lamp, Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});
            world.add(lamp, PointLight{.colour = colour, .intensity = lampIntensity, .range = lampRange, .sourceRadius = lampSourceRadius});
            lampEntities.push_back(lamp);

            Transform pillarPlacement;
            pillarPlacement.position = pillarFoot + Vec3{0.0f, pillarHalfHeight, 0.0f};
            pillarPlacement.scale = Vec3{pillarHalfWidth, pillarHalfHeight, pillarHalfWidth};

            const Entity pillar = world.create();
            world.add(pillar, pillarPlacement);
            world.add(pillar, makeMeshRenderer(cubeMesh, stone));
            world.add(pillar, cubeCollider);
            lampEntities.push_back(pillar);

            ++index;
        }
    }
}

/*
* Runs every frame, after the camera moves, because the camera moves per frame: updating per tick would make the beam lag behind mouse look.
* Normally it is held in the right hand and aimed at the middle of the view.
* In MOBA mode the duck holds it, so it uses the duck's drawn (interpolated) pose, keeping the beam in step with the model.
* It needs no PreviousTransform: it is already placed exactly where things are drawn this frame.
*/
void DemoGame::updateFlashlight(World& world, float alpha) {
    Vec3 position = camera.getPosition() + camera.getRight() * flashlightHandRight - camera.getUp() * flashlightHandDown;
    Vec3 direction = camera.getPosition() + camera.getForward() * flashlightAimDistance - position;

    if (controlMode == ControlMode::Moba && world.isAlive(player)) {
        // Held above the ground, out in front, and tilted down so the beam lands a few steps ahead.
        const Vec3 facing = getCharacterFacing(world, player, alpha);
        position = getRenderTransform(world, player, alpha).position + Vec3{0.0f, flashlightHeight, 0.0f} + facing * flashlightReach;
        direction = facing * std::cos(flashlightTilt) - Vec3{0.0f, std::sin(flashlightTilt), 0.0f};
    }

    world.get<Transform>(flashlight).position = position;

    if (SpotLight* beam = world.tryGet<SpotLight>(flashlight)) {
        beam->direction = direction;
    }
}

/*
* The systems, in a fixed order. The engine has already saved previous transforms for interpolation.
*/
void DemoGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    latestSimulationSeconds = simulationSeconds;

    // Each system timed on its own, so the debug readout shows where a tick goes.
    timeSection(systemTimes, charactersSection, [&] { updateCharacters(world, staticLevel, tickSeconds); });
    timeSection(systemTimes, bodiesSection, [&] { updateFirstPersonBodies(world, staticLevel, tickSeconds); });
    timeSection(systemTimes, spinnersSection, [&] { updateSpinners(world, simulationSeconds); });
    timeSection(systemTimes, firefliesSection, [&] { updateFireflies(world, simulationSeconds); });
    timeSection(systemTimes, particlesSection, [&] { moveParticles(world, staticLevel, tickSeconds); });
    timeSection(systemTimes, grenadesSection, [&] { updateGrenades(world, tickSeconds); });

    // Simulation continues when the cursor is released or focus is lost.
    if (world.isAlive(player) && !world.get<CharacterMovement>(player).isMoving()) {
        world.get<ModelRenderer>(destinationMarker).visible = false;
    }
}

/*
* The camera is the player's view, not game state, so it updates every rendered frame. Mouse look in particular must not be limited to the tick rate.
*/
void DemoGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    const bool modeChanged = updateModeSwitching(world, input);

    // Motion from the previous mode must not rotate the new camera pose.
    if (!modeChanged) {
        updateCamera(world, input, frameSeconds, alpha);
    }

    updateFlashlight(world, alpha);

    // After the camera has moved, so the outline matches what this frame shows under the cursor.
    updateHover(world, input);

    // Once a frame, like the engine's own profiler, so its windows end on time even in frames without a tick.
    systemTimes.update(profilerSeconds());
}

Camera& DemoGame::getCamera() {
    return camera;
}

bool DemoGame::wantsMouseLook() const {
    return controlMode != ControlMode::Moba;
}

/*
* The MOBA camera floats about 16 units from the ground it shows, beyond the reach of a torch-sized light, so lights are
* ranked by their brightness at the ground point in the middle of the screen. The other modes see from among the lights.
*/
Vec3 DemoGame::getLightFocus() {
    if (controlMode == ControlMode::Moba) {
        return mobaCamera.getLookPoint(camera);
    }

    return camera.getPosition();
}

Entity DemoGame::getPlayer() const {
    return player;
}

Entity DemoGame::getDestinationMarker() const {
    return destinationMarker;
}

Entity DemoGame::getFlashlight() const {
    return flashlight;
}

Entity DemoGame::getMoon() const {
    return moon;
}

Entity DemoGame::getSelected() const {
    return selected;
}

Entity DemoGame::getHovered() const {
    return hovered;
}

void DemoGame::setControlMode(World& world, ControlMode mode) {
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    // Each selection starts from a predictable test position.
    switch (mode) {
    case ControlMode::FirstPerson:
        // From the free camera, drop the body where the camera is (it falls onto whatever is below) and keep looking the
        // same way; otherwise look straight ahead from wherever the body stands.
        if (controlMode == ControlMode::FreeCamera) {
            const float eyeHeight = world.get<FirstPersonBody>(firstPersonBody).settings.standingEyeHeight;
            placeFirstPersonBody(world, firstPersonBody, camera.getPosition() - Vec3{0.0f, eyeHeight, 0.0f});
        } else {
            const Vec3 eye = firstPersonEyePosition(world, firstPersonBody, 1.0f);
            camera.setPose(eye, eye + Vec3{0.0f, 0.0f, -1.0f}, worldUp);
        }
        firstPersonCamera.takeOver(camera);
        break;

    case ControlMode::Moba:
        mobaCamera.centreOn(camera, Vec3{0.0f, 0.0f, -6.0f});
        break;

    case ControlMode::FreeCamera:
        camera.setPose(
            freeCameraStart,
            Vec3{0.0f, 0.0f, -5.0f},
            worldUp
        );
        freeFlyCamera.takeOver(camera);
        break;

    default:
        throw std::invalid_argument("Unknown control mode");
    }

    controlMode = mode;
    groundSteeringActive = false;
    if (world.isAlive(player)) {
        world.get<CharacterMovement>(player).stop();
    }
    world.get<ModelRenderer>(destinationMarker).visible = false;
}

bool DemoGame::updateModeSwitching(World& world, const Input& input) {
    if (!enableDebugModeSwitching || !input.hasKeyboardFocus()) {
        return false;
    }

    if (input.wasKeyPressed(Key::F1)) {
        setControlMode(world, ControlMode::FirstPerson);
        return true;
    }

    if (input.wasKeyPressed(Key::F2)) {
        setControlMode(world, ControlMode::Moba);
        return true;
    }

    if (input.wasKeyPressed(Key::F3)) {
        setControlMode(world, ControlMode::FreeCamera);
        return true;
    }

    return false;
}

/*
* Each mode hands the camera to its own controller. Toggles such as the MOBA lock are the demo's rules, so they stay here.
*/
void DemoGame::updateCamera(const World& world, const Input& input, float frameSeconds, float alpha) {
    switch (controlMode) {
    case ControlMode::FirstPerson:
        firstPersonCamera.update(camera, input, frameSeconds);
        camera.setPosition(firstPersonEyePosition(world, firstPersonBody, alpha));
        break;

    case ControlMode::FreeCamera:
        freeFlyCamera.update(camera, input, frameSeconds);
        break;

    case ControlMode::Moba: {
        if (input.isCursorConfined() && input.wasKeyPressed(Key::Space)) {
            mobaCameraLocked = !mobaCameraLocked;
        }

        // Follow where the character is drawn, not where the latest tick put it, so the camera and the character move together smoothly.
        std::optional<Vec3> followTarget;
        if (mobaCameraLocked && world.isAlive(player)) {
            followTarget = getCharacterVisualCentre(world, player, alpha);
        }

        mobaCamera.update(camera, input, frameSeconds, followTarget);
        break;
    }
    }
}

/*
* Turn right-clicks into move commands. A click uses the exact position from its event; holding the button keeps steering towards the live cursor.
*/
void DemoGame::updatePlayerCommands(World& world, const Input& input) {
    // Only accept new commands while MOBA input is active.
    const bool commandsActive = world.isAlive(player) && mobaPointerActive(input);

    if (!commandsActive) {
        groundSteeringActive = false;
        return;
    }

    std::optional<Vec2> target;

    if (const auto press = input.getLastMouseButtonPress(MouseButton::Right)) {
        target = press->position;

        // A tap that was pressed and released within one frame moves once but does not start steering.
        groundSteeringActive = input.isMouseButtonHeld(MouseButton::Right);
    } else if (groundSteeringActive) {
        if (input.isMouseButtonHeld(MouseButton::Right) && input.isCursorInWindow()) {
            target = input.getCursorPosition();
        } else {
            groundSteeringActive = false;
        }
    }

    if (!target) {
        return;
    }

    const std::optional<Ray> ray = rayThroughPixel(input, *target);
    if (!ray) {
        return;
    }

    // Aim at the level (the ground, or the top of a step or platform), not at a flat plane: the duck heads for the
    // point across the ground and the character controller takes it up whatever is in the way. Off the level, nothing.
    const std::optional<LevelHit> hit = raycast(staticLevel, *ray);
    if (!hit) {
        return;
    }
    const Vec3& point = hit->hit.point;

    world.get<CharacterMovement>(player).moveTo(point);

    // The marker has no PreviousTransform, so it jumps straight to the new spot, just above the surface so it is not hidden in it.
    world.get<Transform>(destinationMarker).position = point + Vec3{0.0f, markerLift, 0.0f};
    world.get<ModelRenderer>(destinationMarker).visible = true;
}

/*
* MOBA mouse commands only count while the game has the keyboard and the cursor is confined to the window, so a click
* that merely brings the window back into focus does not also order the duck around.
*/
bool DemoGame::mobaPointerActive(const Input& input) const {
    return controlMode == ControlMode::Moba && input.hasKeyboardFocus() && input.isCursorConfined();
}

/*
* The ray from the camera through a window pixel, or nothing for a pixel outside the window (or a window with no size).
*/
std::optional<Ray> DemoGame::rayThroughPixel(const Input& input, const Vec2& pixel) const {
    const Vec2 size = input.getWindowSize();

    if (size.x <= 0.0f || size.y <= 0.0f ||
        pixel.x < 0.0f || pixel.y < 0.0f ||
        pixel.x >= size.x || pixel.y >= size.y) {
        return std::nullopt;
    }

    return makeCameraRay(camera, pixel.x / size.x, pixel.y / size.y, size.x / size.y);
}

/*
* A left-click selects the nearest collider under the exact click position, tested against triangles where a collider has
* them, so the gap in the teapot's handle is not the teapot. Missing everything (the ground has no collider) deselects.
*/
void DemoGame::updateSelection(World& world, const Input& input) {
    if (!mobaPointerActive(input)) {
        return;
    }

    const auto press = input.getLastMouseButtonPress(MouseButton::Left);
    if (!press) {
        return;
    }

    const std::optional<Ray> ray = rayThroughPixel(input, press->position);
    if (!ray) {
        return;
    }

    selected = pick(world, *ray).entity;
}

/*
* Every frame in MOBA mode, the nearest collider under the live cursor, by its triangles where it has them, like a click.
* The scene is rebuilt first so the answer matches this frame; the grid then lets the ray test only colliders along its way.
*/
void DemoGame::updateHover(const World& world, const Input& input) {
    hovered = Entity{};
    hoveredTriangle.reset();
    hoverStats = CollisionQueryStats{};
    levelHoverStats = CollisionQueryStats{};

    if (!mobaPointerActive(input) || !input.isCursorInWindow()) {
        return;
    }

    const std::optional<Ray> ray = rayThroughPixel(input, input.getCursorPosition());
    if (!ray) {
        return;
    }

    const Pick found = pick(world, *ray, &hoverStats, &levelHoverStats);
    hovered = found.entity;
    hoveredTriangle = found.triangle;
}

/*
* Colliders first (by their triangles where they have them), then the level, which only needs to look as far as the
* collider hit: whichever is nearer is what the ray points at. So a wall hides what stands behind it.
*/
DemoGame::Pick DemoGame::pick(const World& world, const Ray& ray, CollisionQueryStats* colliderStats, CollisionQueryStats* levelStats) {
    collisionScene.rebuild(world);
    const std::optional<ColliderHit> colliderHit = raycast(collisionScene, ray, RaycastDetail::Triangles, unlimitedRayDistance, colliderStats);
    const float colliderDistance = colliderHit ? colliderHit->hit.distance : unlimitedRayDistance;
    const std::optional<LevelHit> levelHit = raycast(staticLevel, ray, colliderDistance, levelStats);

    if (levelHit && (!colliderHit || levelHit->hit.distance < colliderHit->hit.distance)) {
        return Pick{Entity{}, levelHit->triangle};
    }
    return Pick{colliderHit ? colliderHit->entity : Entity{}, std::nullopt};
}

std::optional<std::uint32_t> DemoGame::getHoveredTriangle() const {
    return hoveredTriangle;
}

const StaticLevel& DemoGame::getLevel() const {
    return staticLevel;
}

/*
* One pole just inside each corner of the course's floor slab, standing on it.
*/
std::array<Vec3, floodlightCount> floodlightPoleFeet() {
    const float x = courseHalfWidth - floodlightPoleInset;
    const float z = courseHalfLength - floodlightPoleInset;
    std::array<Vec3, floodlightCount> feet;
    int index = 0;
    for (const float sideX : {-1.0f, 1.0f}) {
        for (const float sideZ : {-1.0f, 1.0f}) {
            feet[index++] = obstacleCourseOrigin + Vec3{sideX * x, obstacleCourseFloorTop, sideZ * z};
        }
    }
    return feet;
}

const std::vector<Entity>& DemoGame::getFloodlights() const {
    return floodlights;
}

/*
* Off means the lamp entities (spotlight and glowing head) are gone, like the lamp field, so they take no light seats
* or shadow tiles; the poles are level geometry and stay. Each lamp hangs out from its pole towards the course, high up,
* and aims at a point past the course's middle, so the four beams cross. Its head glows with the light's colour and does
* not block the light that comes from inside it.
*/
void DemoGame::toggleFloodlights(World& world) {
    if (!floodlights.empty()) {
        for (const Entity light : floodlights) {
            world.destroy(light);
        }
        floodlights.clear();
        return;
    }

    Material glow;
    glow.colour = Pixel{0, 0, 0};
    glow.specularStrength = 0.0f;
    glow.emissive = floodlightColour * floodlightGlowBrightness;

    for (const Vec3& foot : floodlightPoleFeet()) {
        const Vec3 towardsMiddle = Vec3{obstacleCourseOrigin.x - foot.x, 0.0f, obstacleCourseOrigin.z - foot.z};
        Transform placement;
        placement.position = Vec3{foot.x, floodlightHeight, foot.z} + towardsMiddle.normalized() * floodlightHeadOffset;
        placement.scale = floodlightHeadHalfSize;

        const Vec3 aim = obstacleCourseOrigin + towardsMiddle * floodlightAimAcross + Vec3{0.0f, obstacleCourseFloorTop, 0.0f};

        ModelRenderer head = makeMeshRenderer(cubeMesh, glow);
        head.castsShadows = false;

        const Entity light = world.create();
        world.add(light, placement);
        world.add(light, std::move(head));
        world.add(light, SpotLight{
            .colour = floodlightColour,
            .intensity = floodlightIntensity,
            .range = floodlightRange,
            .direction = aim - placement.position,
            .innerAngle = radiansFromDegrees(floodlightInnerDegrees),
            .outerAngle = radiansFromDegrees(floodlightOuterDegrees),
            .sourceRadius = floodlightSourceRadius,
            .emitterRadius = floodlightEmitterRadius
        });
        floodlights.push_back(light);
    }
}

Entity DemoGame::getFirstPersonBody() const {
    return firstPersonBody;
}

/*
* Once a frame, the movement keys become commands for the body's next tick: WASD a wished velocity turned by where the
* camera faces, Shift sprints, Ctrl crouches, and a fresh press of Space jumps (holding it does not jump again on
* landing). A fresh press of G throws a grenade from the eyes along the view. Outside first person, or with the mouse
* released, the body is told to stand still.
*/
void DemoGame::updateFirstPersonCommands(World& world, const Input& input) {
    CharacterController& controller = world.get<CharacterController>(firstPersonBody);
    FirstPersonBody& body = world.get<FirstPersonBody>(firstPersonBody);

    if (controlMode != ControlMode::FirstPerson || !input.isMouseCaptured()) {
        controller.wishVelocity = Vec3{};
        controller.wantsJump = false;
        body.wantsCrouch = false;
        return;
    }

    body.wantsCrouch = input.isKeyHeld(Key::LeftCtrl) || input.isKeyHeld(Key::RightCtrl);
    const bool sprinting = input.isKeyHeld(Key::LeftShift) || input.isKeyHeld(Key::RightShift);
    controller.wishVelocity = firstPersonWishVelocity(getMoveAxes(input), firstPersonCamera.getAngles().yaw, body.crouching,
                                                      sprinting, body.settings);
    if (input.wasKeyPressed(Key::Space)) {
        controller.wantsJump = true;
    }
    if (input.wasKeyPressed(Key::G)) {
        throwGrenade(world, camera.getPosition(), camera.getForward(), grenadeLooks, grenadeSettings);
    }
}
