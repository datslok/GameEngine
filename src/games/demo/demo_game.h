#pragma once

#include "core/profiler.h"
#include "ecs/world.h"
#include "engine/game.h"
#include "gameplay/control_mode.h"
#include "gameplay/flashlight.h"
#include "gameplay/grenade.h"
#include "gameplay/player_controls.h"
#include "input/input.h"
#include "math/ray.h"
#include "physics/collider.h"
#include "physics/collision_scene.h"
#include "physics/static_level.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/light.h"

#include <array>
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

// Floodlight masts at the course's four corners, their lamps this high above the ground; K switches the lamps.
inline constexpr int floodlightCount = 4;
inline constexpr float floodlightHeight = 8.0f;

// Where each floodlight pole stands: its foot, on the course's floor slab, just inside a corner.
std::array<Vec3, floodlightCount> floodlightPoleFeet();

/*
* The demo scene: spinning objects, a ground plane and a duck you can steer by clicking.
* F1/F2/F3 switch between first person, MOBA and free camera when debug mode switching is on.
*/
class DemoGame final : public Game {
public:
    explicit DemoGame(ControlMode startMode = ControlMode::FreeCamera, bool debugModeSwitching = true);

    const char* title() const override;
    void onInit(World& world, AssetManager& assets) override;
    void onInput(World& world, const Input& input) override;
    void onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) override;
    void onUpdate(World& world, const Input& input, float frameSeconds, float alpha) override;
    Camera& getCamera() override;
    bool wantsMouseLook() const override;
    Vec3 getLightFocus() override;
    void onDebugDraw(const World& world, DebugDraw& debug, float alpha) override;
    bool wantsDebugView() const override;
    void visitSettings(SettingsVisitor& visitor) override;
    std::vector<GoldenView> goldenViews() const override;
    void prepareGoldenView(World& world, const GoldenView& view) override;

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

    // The floodlight entities (each a spotlight and its glowing lamp head) while they are on, or none.
    const std::vector<Entity>& getFloodlights() const;

    // The player's body in first person: an invisible capsule that walks through the level, with the camera at its eyes.
    Entity getFirstPersonBody() const;

private:
    void createScene(World& world, AssetManager& assets);

    // Switch the view (PlayerControls) and drop the duck's orders, which belong to the MOBA view it was given in.
    void setControlMode(World& world, ControlMode mode);
    void dropMobaOrders(World& world);

    // Turn right-clicks into move commands for the player.
    void updatePlayerCommands(World& world, const Input& input);

    // Turn the first-person keys into commands: moving the body (PlayerControls) and throwing grenades.
    void updateFirstPersonCommands(World& world, const Input& input);

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

    // The camera and its three views, switched with F1-F3 (a real game would use just one), and the first-person body.
    PlayerControls controls;

    // Entity{} means "none" until onInit creates them.
    Entity player;
    Entity destinationMarker;

    // F toggles it. Held in the right hand, or by the duck in MOBA mode.
    Flashlight flashlight;
    void updateFlashlight(World& world, float alpha);

    // A glowing sphere in the direction the moonlight comes from, at a fixed place you can fly to in free camera mode.
    Entity moon;

    // L toggles a field of 100 coloured lamps, more point lights than the shader has seats for, to show light priority:
    // each lamp flies round a pillar like a firefly, and the pillar throws a moving shadow only while its lamp has one.
    std::vector<Entity> lampEntities;
    MeshHandle sphereMesh;
    MeshHandle cubeMesh;

    // The cube's collider (its box plus its triangles), shared by the spinning cubes and the lamp pillars.
    Collider cubeCollider;
    void toggleLamps(World& world);
    void signalLampField(World& world) const;

    // K switches the floodlights over the obstacle course (on at the start).
    std::vector<Entity> floodlights;
    void toggleFloodlights(World& world);

    // G throws a grenade in first person: a small dark ball that flies, bounces round the level and is gone after a while.
    GrenadeSettings grenadeSettings;
    ModelRenderer grenadeLooks;

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

    // The simulation time of the latest tick, so things created between ticks (the lamps) can start mid-flight.
    double latestSimulationSeconds = 0.0;

    // What each system costs per tick, for the debug readout (shown by onDebugDraw, windows advanced in onUpdate).
    Profiler systemTimes;
    const int charactersSection = systemTimes.addSection("CHARACTERS");
    const int bodiesSection = systemTimes.addSection("BODIES");
    const int spinnersSection = systemTimes.addSection("SPINNERS");
    const int firefliesSection = systemTimes.addSection("FIREFLIES");
    const int particlesSection = systemTimes.addSection("PARTICLES");
    const int grenadesSection = systemTimes.addSection("GRENADES");
};
