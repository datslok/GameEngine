#pragma once

#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/camera/first_person_camera_controller.h"
#include "gameplay/camera/free_fly_camera_controller.h"
#include "gameplay/camera/moba_camera_controller.h"
#include "games/demo/control_mode.h"
#include "input/input.h"
#include "math/ray.h"
#include "physics/collider.h"
#include "physics/collision_scene.h"
#include "physics/static_level.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/light.h"

#include <cstdint>
#include <optional>
#include <vector>

// Where the demo's obstacle course stands (its floor slab's centre at ground level) and the height of the slab's top: the
// course is built around them, and tests aim at them.
inline const Vec3 obstacleCourseOrigin{17.0f, 0.0f, -4.0f};
inline constexpr float obstacleCourseFloorTop = 0.1f;

// The course's stairs: this many steps, each this much higher, up to a platform at obstacleCoursePlatformTop.
inline constexpr int obstacleCourseStairSteps = 5;
inline constexpr float obstacleCourseStairRise = 0.25f;
inline constexpr float obstacleCoursePlatformTop = obstacleCourseFloorTop + obstacleCourseStairRise * obstacleCourseStairSteps;

/*
* The demo scene: spinning objects, a ground plane and a duck you can steer by clicking.
* F1/F2/F3 switch between first person, MOBA and free camera when debug mode switching is on.
*/
class DemoGame final : public Game {
public:
    explicit DemoGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

    void onInit(World& world, AssetManager& assets) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool wantsMouseLook() const override;
    Vec3 getLightFocus() override;
    void onDebugDraw(const World& world, DebugDraw& debug, float alpha) override;
    bool wantsDebugView() const override;

    // For tests and debugging.
    Entity getPlayer() const;
    Entity getDestinationMarker() const;
    Entity getFlashlight() const;
    Entity getMoon() const;

    // What the last left-click in MOBA mode picked, or Entity{} for nothing.
    Entity getSelected() const;

    // What the cursor is over in MOBA mode, updated every frame, or Entity{} for nothing.
    Entity getHovered() const;

    // The level triangle the cursor is over in MOBA mode (when no collider is nearer), or nothing.
    std::optional<std::uint32_t> getHoveredTriangle() const;

    // The demo's static level: the ground and the obstacle course.
    const StaticLevel& getLevel() const;

private:
    void createScene(World& world, AssetManager& assets);
    void setControlMode(World& world, ControlMode mode);

    // Turn right-clicks into move commands for the player.
    void updatePlayerCommands(World& world, const Input& input);

    // Turn left-clicks into a selection: the nearest collider under the cursor, or none.
    void updateSelection(World& world, const Input& input);

    // Find what the cursor is over (MOBA mode only), through the collision scene, every frame.
    void updateHover(const World& world, const Input& input);

    // What a ray points at: the nearest collider entity, or a level triangle when the level is nearer (or neither).
    struct Pick {
        Entity entity;
        std::optional<std::uint32_t> triangle;
    };
    Pick pick(const World& world, const Ray& ray, CollisionQueryStats* colliderStats = nullptr, CollisionQueryStats* levelStats = nullptr);

    // Whether MOBA mouse commands count right now: MOBA mode, keyboard focus and a confined cursor.
    bool mobaPointerActive(const Input& input) const;

    // The camera ray through a window pixel, or nothing outside the window.
    std::optional<Ray> rayThroughPixel(const Input& input, const Vec2& pixel) const;

    // Returns true when the mode changed, so this frame's mouse motion is not applied to the new camera pose.
    bool updateModeSwitching(World& world, const Input& input);

    // Run the controller for the current mode.
    void updateCamera(const World& world, const Input& input, float frameSeconds, float alpha);

    Camera camera;

    // A real game would use just one of these. The demo keeps all three and switches with F1-F3.
    FirstPersonCameraController firstPersonCamera;
    MobaCameraController mobaCamera;
    FreeFlyCameraController freeFlyCamera;

    ControlMode controlMode;
    bool enableDebugModeSwitching;

    // Entity{} means "none" until onInit creates them.
    Entity player;
    Entity destinationMarker;

    // F toggles it. Held at the camera, or by the duck in MOBA mode.
    Entity flashlight;
    // A large source radius keeps it gentle up close; the higher intensity carries it further.
    SpotLight flashlightBeam{
        .colour = Vec3{1.0f, 0.89f, 0.69f},
        .intensity = 12.0f,
        .range = 50.0f,
        .sourceRadius = 4.5f
    };
    void toggleFlashlight(World& world);
    void updateFlashlight(World& world, float alpha);

    // A glowing sphere in the direction the moonlight comes from, at a fixed place you can fly to in free camera mode.
    Entity moon;

    // L toggles a field of coloured lamps, far more point lights than the shader has seats for, to show light priority:
    // each lamp flies round a pillar like a firefly, and the pillar throws a moving shadow only while its lamp has one.
    std::vector<Entity> lampEntities;
    MeshHandle sphereMesh;
    MeshHandle cubeMesh;

    // The cube's collider (its box plus its triangles), shared by the spinning cubes and the lamp pillars.
    Collider cubeCollider;
    void toggleLamps(World& world);

    // True while a right-click that started in MOBA mode is still held, so the player keeps following the cursor.
    bool groundSteeringActive = false;

    // What the last left-click picked (Entity{} for nothing), outlined every frame.
    Entity selected;

    // What the cursor is over (Entity{} for nothing), outlined faintly, and what finding it cost (for the F4 readout).
    Entity hovered;
    CollisionQueryStats hoverStats;

    // The level triangle the cursor is over (when no collider is nearer), and what finding it cost.
    std::optional<std::uint32_t> hoveredTriangle;
    CollisionQueryStats levelHoverStats;

    // The ground and the obstacle course: built once in createScene, never rebuilt.
    StaticLevel staticLevel;

    // Every collider, rebuilt before each use (once per frame for hovering, and on a click), so it is never stale.
    CollisionScene collisionScene;

    // F4: show bounding boxes, the culling readout, light markers and the walk target.
    bool debugViewEnabled = false;

    // Space toggles following the player in MOBA mode.
    bool mobaCameraLocked = false;

    // The simulation time of the latest tick, so things created between ticks (the lamps) can start mid-flight.
    double latestSimulationSeconds = 0.0;
};
