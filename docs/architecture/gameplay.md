# Gameplay and the demo

Covers: `src/gameplay/`, `src/scene/camera`, `src/scene/camera_ray`, `src/games/demo/`

Characters, the first-person body, cameras and their controllers, control modes (F1-F3), `PlayerControls`, pointer rays, grenades and the demo game's scene and rules. The physics underneath (controller, particles, picking): `physics.md`. Movement tuning rule: `.claude/rules/character-tuning.md`.

## Characters

`spawnCharacter(world, assets, model, config, groundPosition)` (`gameplay/character`) makes an entity with `Transform` (position = feet; the model lifted so its lowest point touches), `PreviousTransform`, `ModelRenderer`, `CharacterMovement`, a `Collider` capsule fitted to the drawn model (radius half its wider horizontal side, feet to head, no detail) and a `CharacterController` from `CharacterConfig::controller`.

- `CharacterConfig` (`gameplay/character_config`): model path and size, speed, turn speed, `modelForwardYaw` (from +Z towards +X; the duck faces +X, pi / 2), controller settings. Throws for a capsule with radius ≤ 0 or height < two radii, a bad turn speed or yaw, a non-finite position.
- `CharacterMovement`: a `MoveToController` (`gameplay/move_to_controller`) plus turning state; `moveTo`, `stop`, `isMoving`. `yaw` is the character's own angle: set it, not the `Transform`, to face somewhere; `targetYaw` survives arrival so the turn finishes.

**`updateCharacters(world, level, dt)`**, per character:

1. `wishVelocity` = `MoveToController::desiredVelocity(position, dt)`: across the ground at full speed, cut on the last step to land exactly; zero with no target or no time.
2. `moveCharacter` (`physics.md`).
3. `updateArrival`: stops within `arrivalDistance` (1e-4). A wall in the way means it never arrives: no path finding.
4. `yaw` turned towards the displacement at `turnSpeed` (wrapped by `wrapAngle`), then written to the rotation as a turn about +Y.

**Helpers:** `getCharacterVisualCentre(world, entity, alpha)` (interpolated camera target), `getCharacterFacing` (the model's forward turned by the interpolated rotation).

## First-person body

`gameplay/first_person_body`. `spawnFirstPersonBody` makes `Transform` (feet), `PreviousTransform`, `CharacterController` (height from the move settings) and `FirstPersonBody`. No `Collider`, so picking never finds it.

- `FirstPersonMoveSettings`: walk 5.5, backward x0.9, backward diagonal x0.93333, crouch 3, sprint x1.5 (Overwatch's measured speeds plus a sprint); standing 1.8 / eyes 1.6, crouching 1.2 / 1.0; eyes ease at `eyeHeightSpeed` (6 units a second).
- `FirstPersonBody`: command `wantsCrouch`; state `crouching`, eased `eyeHeight`.
- `firstPersonWishVelocity(axes, yaw, crouching, sprinting, settings)`: `getMoveAxes` turned by yaw, normalised (diagonals no faster), scaled by the direction's speed.
- `updateFirstPersonBodies(world, level, seconds)`: crouch at once; stand only if `characterFits` the standing capsule; ease the eyes; `moveCharacter`.
- `firstPersonEyePosition(world, body, alpha)`: interpolated feet + eye height.
- `placeFirstPersonBody`: moves `Transform` and `PreviousTransform`, clears velocity, ground looked up afresh (no streak, no carried speed).

## Cameras

`Camera` (`scene/camera`): a plain lens (position, forward, up, vertical FOV, aspect, near, far); view and projection built on request, so `setVerticalFov` works for zoom. No movement logic. `makeCameraRay` (`scene/camera_ray`) turns a normalised pixel into a world ray; `intersectGround` (`math/ray`) meets y = 0.

Controllers (`gameplay/camera/`) are concrete classes with no shared interface; each owns settings with a `visit`, read every frame.

| Controller | Does | Settings |
|---|---|---|
| `FreeFlyCameraController` | Mouse look; WASD along the view, Space and Ctrl up and down, Shift faster | `look`, `moveSpeed` 3 |
| `FirstPersonCameraController` | Mouse look only, every frame; the game puts it at the body's eyes | `look` |
| `MobaCameraController` | Fixed-angle offset; follows a target if given, else edge-pans while the cursor is confined (`gameplay/edge_pan`) | `offset` (0, 12, 10), `panSpeed` 9, `edgeMarginPixels` 5 |

- **Look** (`look_controls`): `ViewAngles` (yaw 0 = -Z, positive turns right; pitch clamped to `maxPitchRadians`, 89 degrees). Angles, not a vector, so the pitch limit is exact. Look controllers pause while the mouse is not captured; `takeOver(camera)` continues from the camera's direction.
- `turnCamera` is shared by both look controllers, `moveCamera` by the free camera only: walking and its sprint belong to the first-person body.
- **`LookSettings`:** `sensitivity` (default 5, slider 1-50, limits 0-100) and `sprintMultiplier` (2, the free camera's Shift).
- **Sensitivity scale:** Overwatch's. `radiansPerCount` = `sensitivity` x `overwatchDegreesPerCount` (0.0066 degrees) per mouse count, both axes, any field of view. SDL's relative mode reports raw counts, unscaled by Windows' pointer speed, as Overwatch reads them, so a player's Overwatch number turns the same angle for the same hand motion.

## Control modes and `PlayerControls`

`ControlMode` (`gameplay/control_mode`): F1 first person, F2 MOBA, F3 free camera.

`PlayerControls` (`gameplay/player_controls`): held by a game as a member (composition: a game wanting other controls holds none). Holds the `Camera`, the three controllers, the mode, the MOBA lock and the first-person body. Built from a `PlayerControlsSetup` (free camera pose, MOBA look point, body's feet, field of view, near and far), a start mode and `debugModeSwitching`.

| Call | Does |
|---|---|
| `spawnBody` | Makes the body at the setup's feet; once, from `onInit`, before the first mode change. `setFirstPersonFeet` moves that start first |
| `setControlMode` | First person from the free camera drops the body an eye height below the camera, keeping the view; from elsewhere looks down -Z from the body. MOBA centres on the look point. Free camera goes to the setup's pose |
| `updateModeSwitching` | F1-F3 with debug switching on and keyboard focus; true when the mode changed, so this frame's mouse motion is not applied |
| `updateCamera(world, input, frameSeconds, alpha, mobaFollowTarget)` | Runs the mode's controller; in MOBA, Space (cursor confined) toggles locking onto the target |
| `updateFirstPersonCommands` | In first person with the mouse captured: `wishVelocity`, `wantsJump` on a fresh Space, `wantsCrouch` while Ctrl is held, Shift sprints; otherwise clears them. Returns whether first-person input counted |
| `poseFreeCamera` | Into the free camera at a pose (golden views, tests, the `camera` command) |
| `wantsMouseLook` | Captured mouse in first person and free camera; visible confined cursor in MOBA |
| `rayThroughPixel(input, pixel)` | World ray through a window pixel (top-left origin), none outside the window |
| `pointerRay(input)` | Through the crosshair while the mouse looks, through the cursor in MOBA; the cloth lab drags with it |
| `getLightFocus` | The MOBA look point (its camera floats beyond a torch's reach), else the camera |
| `visitSettings` | Groups "First person camera", "MOBA camera", "Free camera" |

What a mode change means for the game's own things stays in the game (the demo's `dropMobaOrders`).

## Small gameplay components

| Component | File | Does |
|---|---|---|
| `Spinner` | `gameplay/spinner` | Euler angles growing at constant speeds, posed from total simulation time |
| `Firefly` | `gameplay/firefly` | `fireflyPosition(firefly, seconds)`: a circle round a centre whose radius, height and speed wobble |
| `Flashlight` | `gameplay/flashlight` | A `SpotLight` entity (`flashlightBeam`, `fast`), removed when off; each switch a probe scene event; `flashlightInHand` places it right of and below the eye, aimed 8 ahead, every frame after the camera |
| `Grenade` | `gameplay/grenade` | `secondsLeft`; `GrenadeSettings` (throw speed 15, lifetime 5 s, `ParticleSettings`) |

- `throwGrenade(world, from, direction, looks, settings)`: `Transform` scaled to the particle radius, `PreviousTransform`, the looks, a unit `Sphere` `Collider`, a `Particle` along the normalised direction (thrower's velocity not added, so throws are predictable), a `Grenade`.
- `updateGrenades(world, seconds)` destroys expired ones inside its `each` (deferred by the `World`). No explosion yet.

## The demo game

`DemoGame` (`games/demo/`, `--game demo`): night scene; the registry starts it in first person with mode switching on (the constructor defaults to the free camera). Holds `PlayerControls`, the duck, the scene, a `StaticLevel` and a `CollisionScene`. Its lighting (moon, flashlight placement, lamp field L, floodlights K, probes): `lights.md`.

**Scene** (`createScene`):

- Spinners with colliders: a textured cube, a smaller one (both sharing `cubeCollider`, a mesh collider), a green pyramid and a gold teapot (their own mesh colliders).
- A 40 m square grass plane (`Mesh::plane(20)`, a half-size), static, no collider (clicking it deselects); the moon sphere with a unit `Sphere` collider; a row of glTF test models (normal maps, emissive, cut-outs) with mesh colliders.
- The obstacle course (`buildObstacleCourse`): cube parts of one entity at `obstacleCourseOrigin` (17, 0, -4), slab top `obstacleCourseFloorTop` (0.1): a wall with a doorway, five 0.25 m steps to a platform, ramps of 20 (walkable) and 50 degrees, the floodlight poles. Static; its collision mesh from the same parts.
- `staticLevel` = ground + course (`getLevel`).
- The duck: `assets/models/Duck.glb` by `spawnCharacter`, radius 0.5, `groundAcceleration` `mobaUnitAcceleration` (1000) so it starts and stops within a tick and lands exactly where sent.
- The yellow destination marker (no `PreviousTransform`: it jumps), `markerLift` 0.02 above the hit.

**Per frame:**

- `onInput`: `updatePlayerCommands`, `updateFirstPersonCommands`, `updateSelection`, then F (flashlight), L (lamps), K (floodlights), F4 (debug view).
- `onUpdate`: mode switching (a change calls `dropMobaOrders`: duck stops, marker hidden, ground steering ends) or the camera (MOBA lock follows the duck's visual centre); the flashlight; `updateHover` after the camera so the outline matches the frame; `systemTimes.update`.
- `onFixedUpdate`, each system timed into `systemTimes`: characters, bodies, spinners, fireflies, particles, grenades; the marker hides once the duck stops.

**MOBA input** counts only with keyboard focus and a confined cursor (`mobaPointerActive`), so a click that refocuses the window orders nothing.

- **Right-click to move** (`updatePlayerCommands`): the ray from the click's exact event position hits the static level, so a step or platform top is the target; off the level, nothing. Holding steers towards the live cursor (`groundSteeringActive`); a tap within one frame moves once.
- **Left-click to select** (`updateSelection`): `pick` at the exact click position; nothing hit deselects (`getSelected`).
- **Hover** (`updateHover`): `pick` under the live cursor; `getHovered` (entity), `getHoveredTriangle` (level triangle).
- **`pick`:** rebuilds the collision scene, raycasts it in `Triangles` mode (so the teapot handle's gap is not the teapot), then the level up to that distance; the nearer wins, so walls hide what is behind them.
- Selection outlined cyan and hover dimmed cyan by `onDebugDraw`, even without F4.

**First person:** a fresh G throws a grenade from the camera along its forward (`grenadeSettings`, `grenadeLooks`, a dark sphere) while first-person input counts.

**F4 lines** (below the engine's): system timings (averages, three decimals); colliders, tested, cells walked; level triangles, tested, cells; the duck's controller (grounded or airborne, slope, substeps) with its ground normal at its feet; the hovered triangle outlined with its normal; light markers and the walk line.

**Settings:** the three cameras' groups and "Grenade".

## Readout lines and profiling

- `writeReadout(debug, text, colour)` (`engine/debug_views`): the next free line down the top-left (`DebugDraw::takeReadoutLine`, reset each frame), wrapped to the screen (`wrapReadout` breaks only between items two spaces apart; a text ending in `readoutUnit`, "MS", ends each line with it; width from `readoutCharactersFitting` and `DebugDraw::getScreenWidth`). Later lines move down.
- The engine writes its lines first (render stats, `cameraText`, CPU and GPU timings); a game's `onDebugDraw` writes below.
- `Profiler` (`core/profiler`, plain C++): numbered sections (`addSection`, `record`), averaged per call over a window (`defaultProfileWindowSeconds`, 0.5) into `SectionTiming` (average, worst, calls) on `update(now)` once a frame; zeros for a section that did not run. `ScopedTimer`, `timeSection`, `profilerSeconds`. Drawn by `drawProfile` (`profileText`).

## Tests

`character_test`, `move_to_controller_test`, `first_person_body_test`, `camera_controller_test`, `edge_pan_test`, `player_controls_test`, `spinner_test`, `firefly_test`, `flashlight_test`, `grenade_test`, `demo_game_test` (helpers: `tests/demo_helpers.h`).
