#include "games/clothlab/cloth_lab_game.h"

#include "core/srgb.h"
#include "games/clothlab/cloth_lab_layout.h"
#include "gameplay/first_person_body.h"
#include "math/transform.h"
#include "scene/atmosphere.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/probe_volume.h"
#include "scene/sun.h"

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
}

void ClothLabGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    (void)simulationSeconds;
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

void ClothLabGame::visitSettings(SettingsVisitor& visitor) {
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
