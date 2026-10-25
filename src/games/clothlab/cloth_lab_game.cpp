#include "games/clothlab/cloth_lab_game.h"

#include "core/srgb.h"
#include "games/clothlab/cloth_lab_layout.h"
#include "gameplay/first_person_body.h"
#include "math/transform.h"
#include "math/angles.h"
#include "physics/collider.h"
#include "scene/atmosphere.h"
#include "scene/deforming_mesh.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/probe_volume.h"
#include "scene/sun.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
    // The views' starts: the free camera behind the player looking at the stations, the MOBA view over the mat, the body
    // a few steps back from them.
    const PlayerControlsSetup clothLabViews{
        .freeCameraPosition = Vec3{0.0f, 2.4f, 6.5f},
        .freeCameraTarget = Vec3{0.0f, 1.0f, -1.0f},
        .mobaLookPoint = Vec3{0.0f, 0.0f, -1.0f},
        .firstPersonFeet = clothLabFirstPersonFeet,
        .verticalFovDegrees = 70.0f,
        .nearPlane = 0.05f,
        .farPlane = 200.0f
    };

    // Surfaces in sRGB, plain and matte so the cloth stands out: a pale concrete floor, a dark blue mat, a grey ball and
    // pedestal, brushed steel frames.
    const Pixel floorColour{160, 158, 152};

    Material floorMaterial() {
        Material material;
        material.colour = floorColour;
        material.roughness = 0.85f;
        return material;
    }

    Material matMaterial() {
        Material material;
        material.colour = Pixel{40, 60, 110};
        material.roughness = 1.0f;
        return material;
    }

    Material stoneMaterial() {
        Material material;
        material.colour = Pixel{190, 188, 182};
        material.roughness = 0.6f;
        return material;
    }

    Material steelMaterial() {
        Material material;
        material.colour = Pixel{180, 182, 186};
        material.metallic = 1.0f;
        material.roughness = 0.35f;
        return material;
    }

    // The line from the held point to the hand, while dragging.
    const Vec3 grabLineColour{1.0f, 0.8f, 0.2f};

    // Cotton seen from both sides: the hanging sheet deep red, the drape sheet cream.
    Material cottonMaterial(const Pixel& colour) {
        Material material;
        material.colour = colour;
        material.roughness = 0.9f;
        material.doubleSided = true;
        return material;
    }

    const Pixel hangingSheetColour{150, 30, 35};
    const Pixel drapeSheetColour{225, 215, 190};
    const Pixel bigSheetColour{150, 165, 190};

    // A grid laid flat (the grid faces +z; turned to face up, its top edge at the back) with its middle at centre.
    Mat4 laidFlat(const Vec3& centre) {
        return Mat4::translation(centre.x, centre.y, centre.z) * Mat4::rotationX(-0.25f * fullTurnRadians);
    }

    // The two corners of a grid's top edge, which lies at the back once laid flat.
    std::array<std::uint32_t, 2> backCorners(int columns, int rows) {
        const std::uint32_t backEdge = static_cast<std::uint32_t>(rows) * static_cast<std::uint32_t>(columns + 1);
        return {backEdge, backEdge + static_cast<std::uint32_t>(columns)};
    }

    /*
    * A cloth as an entity: drawn through a deforming mesh of its own, in world space (its points are where the cloth
    * is), the cloth on the same entity.
    */
    Entity addSheet(World& world, AssetManager& assets, const Mesh& mesh, Cloth cloth, const Material& material) {
        DeformingMesh shape;
        shape.mesh = assets.addDeformingMesh(mesh);
        shape.positions = cloth.particles.positions;
        ModelRenderer renderer;
        renderer.parts.push_back(RenderPart{shape.mesh, material, Mat4::identity()});
        const Entity entity = world.create();
        world.add(entity, Transform{});
        world.add(entity, std::move(renderer));
        world.add(entity, std::move(shape));
        world.add(entity, std::move(cloth));
        return entity;
    }

    // A body cloth collides with: a Collider at the origin, its shape already in the world.
    void addBody(World& world, const Shape& shape) {
        const Entity entity = world.create();
        world.add(entity, Transform{});
        world.add(entity, Collider{shape, nullptr});
    }

    // One box as a part: the unit cube (-1..1) stretched to its half-size and moved.
    RenderPart blockPart(MeshHandle cube, const ClothLabBlock& block, const Material& material) {
        return RenderPart{cube, material, Mat4::translation(block.centre.x, block.centre.y, block.centre.z) *
                                              Mat4::scaling(block.halfSize.x, block.halfSize.y, block.halfSize.z)};
    }

    // The golden image views (engine/golden.h), from the free camera: every station from behind the player.
    struct ClothLabGoldenView {
        GoldenView view;
        Vec3 cameraPosition;
        Vec3 cameraTarget;
    };

    const ClothLabGoldenView clothLabGoldenViews[] = {
        {{"stations", 0.5, false}, clothLabViews.freeCameraPosition, clothLabViews.freeCameraTarget}
    };
}

ClothLabGame::ClothLabGame(ControlMode startMode, bool debugModeSwitching):
    controls(clothLabViews, startMode, debugModeSwitching)
{
}

const char* ClothLabGame::title() const {
    return "Cloth lab";
}

/*
* Everything built here is static level: the triangles drawn are the triangles walked on, made from the same renderers,
* and the light probes move off them (ModelRenderer::isStatic). The cloth, when it comes, will be the only thing that moves.
*/
void ClothLabGame::onInit(World& world, AssetManager& assets) {
    const MeshHandle cube = assets.addMesh(Mesh::cube());
    const ClothLabLayout layout = buildClothLabLayout();

    const auto addStatic = [&](ModelRenderer renderer) {
        renderer.isStatic = true;
        staticLevel.add(assets.makeCollisionMesh(renderer));
        const Entity entity = world.create();
        world.add(entity, Transform{});
        world.add(entity, std::move(renderer));
    };

    addStatic(makeMeshRenderer(assets.addMesh(Mesh::plane(clothLabFloorHalfSize)), floorMaterial()));

    ModelRenderer stations;
    stations.parts.push_back(blockPart(cube, layout.pedestal, stoneMaterial()));
    stations.parts.push_back(blockPart(cube, layout.mat, matMaterial()));
    for (const ClothLabFrame& frame : {layout.drapeFrame, layout.fabricFrame}) {
        for (const ClothLabBlock& block : clothLabFrameBlocks(frame)) {
            stations.parts.push_back(blockPart(cube, block, steelMaterial()));
        }
    }

    // The ball, finely divided so a sheet's folds over it are not drawn against a faceted outline.
    const float radius = layout.ball.radius;
    stations.parts.push_back(RenderPart{assets.addMesh(Mesh::sphere(64, 32)), stoneMaterial(),
                                        Mat4::translation(layout.ball.centre.x, layout.ball.centre.y, layout.ball.centre.z) *
                                            Mat4::scaling(radius, radius, radius)});
    addStatic(std::move(stations));

    // The bodies cloth collides with: the ball, every block, and the floor as a thick box under the ground.
    addBody(world, Shape{layout.ball});
    for (const ClothLabBlock& block : layout.blocks()) {
        addBody(world, Shape{block.bounds()});
    }
    addBody(world, Shape{Aabb{Vec3{-clothLabFloorHalfSize, -clothLabFloorDepth, -clothLabFloorHalfSize}, Vec3{clothLabFloorHalfSize, 0.0f, clothLabFloorHalfSize}}});

    // The hanging sheet: laid flat level with the fabric bar, hung from two hooks at its back corners.
    const Mesh sheetMesh = Mesh::grid(clothLabSheetColumns, clothLabSheetRows, clothLabSheetWidth, clothLabSheetDepth);
    Cloth hanging = makeCloth(sheetMesh, laidFlat(layout.sheetCentre), clothSettings);
    for (std::uint32_t corner : backCorners(clothLabSheetColumns, clothLabSheetRows)) {
        addClothPin(hanging, corner);
    }
    hookMiddle = (hanging.particles.positions[hanging.pins[0].particle] + hanging.particles.positions[hanging.pins[1].particle]) * 0.5f;
    sheet = addSheet(world, assets, sheetMesh, std::move(hanging), cottonMaterial(hangingSheetColour));

    // The drape sheet: laid flat level with the drape bar over the ball, pinned at its back corners.
    const Mesh drapeMesh = Mesh::grid(clothLabDrapeSheetSquares, clothLabDrapeSheetSquares, clothLabDrapeSheetSize, clothLabDrapeSheetSize);
    Cloth draping = makeCloth(drapeMesh, laidFlat(layout.drapeSheetCentre), clothSettings);
    for (std::uint32_t corner : backCorners(clothLabDrapeSheetSquares, clothLabDrapeSheetSquares)) {
        pinParticle(draping.particles, corner);
    }
    drapeSheet = addSheet(world, assets, drapeMesh, std::move(draping), cottonMaterial(drapeSheetColour));

    // The big sheet: laid flat over the mat, its four corners on hooks that draw in to their span during the first tick.
    if (stationSettings.bigSheet) {
        const int squares = clothLabBigSheetSquares;
        const Mesh bigMesh = Mesh::grid(squares, squares, clothLabBigSheetSize, clothLabBigSheetSize);
        Cloth big = makeCloth(bigMesh, laidFlat(layout.bigSheetCentre), clothSettings);
        const std::uint32_t row = static_cast<std::uint32_t>(squares + 1);
        for (std::uint32_t corner : {0u, static_cast<std::uint32_t>(squares), static_cast<std::uint32_t>(squares) * row,
                                     static_cast<std::uint32_t>(squares) * row + static_cast<std::uint32_t>(squares)}) {
            addClothPin(big, corner);
            ClothPin& pin = big.pins.back();
            const Vec3 out = pin.position - layout.bigSheetCentre;
            const float half = 0.5f * clothLabBigSheetCornerSpan;
            pin.position = layout.bigSheetCentre + Vec3{out.x < 0.0f ? -half : half, 0.0f, out.z < 0.0f ? -half : half};
        }
        bigSheet = addSheet(world, assets, bigMesh, std::move(big), cottonMaterial(bigSheetColour));
    }

    controls.spawnBody(world);

    const Vec3 towardsSun = directionTowardsSunOn(clothLabLatitudeDegrees, clothLabDayOfYear, clothLabSolarHours);
    DirectionalLight sun;
    sun.direction = towardsSun * -1.0f;
    sun.intensity = clothLabSunIntensity;
    world.add(world.create(), sun);

    // The real sky, its ground seen past the floor's edge the floor's colour (in linear light, as material colours are
    // decoded), and light probes round the camera.
    Atmosphere atmosphere;
    atmosphere.groundAlbedo = Vec3{srgbByteToLinear(floorColour.r), srgbByteToLinear(floorColour.g), srgbByteToLinear(floorColour.b)};
    const Entity air = world.create();
    world.add(air, atmosphere);
    world.add(air, ProbeCascades{});

    controls.setControlMode(world, controls.getMode());
}

void ClothLabGame::onInput(World& world, const Input& input) {
    controls.updateFirstPersonCommands(world, input);

    if (input.wasKeyPressed(Key::F4)) {
        debugViewEnabled = !debugViewEnabled;
    }

    updateGrab(world, input);
}

/*
* Once a frame, from last frame's camera: a press grabs the corner nearest where the pointer meets the sheet, within
* reach; while held, the hand follows the pointer at the grab's distance (turning or walking carries the cloth, as at
* arm's length); a release, or a pointer that leaves the window, lets go. The ticks only read the result. While the mouse
* should look but is free (after Escape), the click that captures it again grabs nothing.
*/
void ClothLabGame::updateGrab(const World& world, const Input& input) {
    const bool pointing = !controls.wantsMouseLook() || input.isMouseCaptured();
    const std::optional<Ray> pointer = pointing ? controls.pointerRay(input) : std::nullopt;

    if (input.wasMouseButtonPressed(MouseButton::Left) && pointer) {
        const Ray ray{pointer->origin, pointer->direction.normalized()};
        float nearest = clothLabGrabReach;
        for (const Entity candidate : sheets()) {
            if (const std::optional<ClothHit> hit = raycastCloth(world.get<Cloth>(candidate), ray, nearest)) {
                sheetGrab = ClothGrab{hit->particle, hit->point};
                grabbedSheet = candidate;
                grabDistance = hit->distance;
                nearest = hit->distance;
            }
        }
    }

    if (!input.isMouseButtonHeld(MouseButton::Left) || !pointer) {
        sheetGrab.reset();
        return;
    }

    if (sheetGrab) {
        sheetGrab->target = pointer->origin + pointer->direction.normalized() * grabDistance;
    }
}

/*
* The fabric's settings reach the sheet every tick, so the debug UI's changes apply at once (its mass per area only when
* it is made). The hooks step towards their gap at their speed, in ticks, so where they are depends only on the ticks.
*/
void ClothLabGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    (void)simulationSeconds;
    for (const Entity each : sheets()) {
        Cloth& cloth = world.get<Cloth>(each);
        cloth.settings = clothSettings;
        cloth.grab = each == grabbedSheet ? sheetGrab : std::nullopt;
    }

    Cloth& cloth = world.get<Cloth>(sheet);

    const float wanted = std::clamp(hookSettings.hookGap, clothLabSmallestHookGap, clothLabSheetWidth);
    const float step = clothLabHookSpeed * tickSeconds;
    hookGapNow += std::clamp(wanted - hookGapNow, -step, step);
    cloth.pins[0].position = hookMiddle - Vec3{0.5f * hookGapNow, 0.0f, 0.0f};
    cloth.pins[1].position = hookMiddle + Vec3{0.5f * hookGapNow, 0.0f, 0.0f};
    stepCloths(world, tickSeconds, getJobs());
    updateFirstPersonBodies(world, staticLevel, tickSeconds);
}

void ClothLabGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    // Motion from the previous mode must not rotate the new camera pose.
    if (!controls.updateModeSwitching(world, input)) {
        controls.updateCamera(world, input, frameSeconds, alpha);
    }
}

Camera& ClothLabGame::getCamera() {
    return controls.getCamera();
}

bool ClothLabGame::wantsMouseLook() const {
    return controls.wantsMouseLook();
}

Vec3 ClothLabGame::getLightFocus() {
    return controls.getLightFocus();
}

bool ClothLabGame::wantsDebugView() const {
    return debugViewEnabled;
}

void ClothLabGame::onDebugDraw(const World& world, DebugDraw& debug, float alpha) {
    (void)alpha;
    if (sheetGrab) {
        debug.line(world.get<Cloth>(grabbedSheet).particles.positions[sheetGrab->particle], sheetGrab->target, grabLineColour);
    }
}

void visit(ClothLabHookSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Hook gap", settings.hookGap, {.min = clothLabSmallestHookGap, .max = clothLabSheetWidth, .unit = "m"});
}

void visit(ClothLabStationSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Big sheet", settings.bigSheet, FieldApplies::AtStartup);
}

void ClothLabGame::visitSettings(SettingsVisitor& visitor) {
    visitor.group("Stations", [&] {
        visit(stationSettings, visitor);
    });
    visitor.group("Cloth", [&] {
        visit(clothSettings, visitor);
    });
    visitor.group("Hooks", [&] {
        visit(hookSettings, visitor);
    });
    controls.visitSettings(visitor);
}

std::vector<GoldenView> ClothLabGame::goldenViews() const {
    std::vector<GoldenView> views;

    for (const ClothLabGoldenView& golden : clothLabGoldenViews) {
        views.push_back(golden.view);
    }

    return views;
}

void ClothLabGame::prepareGoldenView(World& world, const GoldenView& view) {
    for (const ClothLabGoldenView& golden : clothLabGoldenViews) {
        if (golden.view.name == view.name) {
            controls.poseFreeCamera(world, golden.cameraPosition, golden.cameraTarget);
        }
    }
}

const StaticLevel& ClothLabGame::getLevel() const {
    return staticLevel;
}

Entity ClothLabGame::getSheet() const {
    return sheet;
}

Entity ClothLabGame::getDrapeSheet() const {
    return drapeSheet;
}

Entity ClothLabGame::getGrabbedSheet() const {
    return grabbedSheet;
}

Entity ClothLabGame::getBigSheet() const {
    return bigSheet;
}

ClothLabStationSettings& ClothLabGame::getStationSettings() {
    return stationSettings;
}

std::vector<Entity> ClothLabGame::sheets() const {
    std::vector<Entity> all{sheet, drapeSheet};
    if (stationSettings.bigSheet) {
        all.push_back(bigSheet);
    }
    return all;
}

ClothSettings& ClothLabGame::getClothSettings() {
    return clothSettings;
}

ClothLabHookSettings& ClothLabGame::getHookSettings() {
    return hookSettings;
}

PlayerControls& ClothLabGame::getControls() {
    return controls;
}

const std::optional<ClothGrab>& ClothLabGame::getSheetGrab() const {
    return sheetGrab;
}
