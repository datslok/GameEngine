# Roadmap

A from-scratch C++20 engine that can host both an **FPS** and a **MOBA**, and eventually a **dressmaking game** with real cloth physics (like Dressmaker, but more realistic), and a **solar system** simulated at real scale. The core stays genre-agnostic; genre code (camera styles, weapons, abilities, netcode tuning, cloth and sewing, orbital mechanics) lives in modules on top.

Each phase ends with a **milestone**: something you can run and see. A phase is done when its milestone works and has tests where it makes sense.

**Principles**
- The simulation runs headless: no SDL or GPU types inside it, so it can run in tests and on a dedicated server.
- The simulation advances in fixed ticks (120 Hz). Rendering interpolates between ticks.
- One branch per feature, tests alongside the code.
- Build it yourself first to learn how it works; reach for a library when the learning is done or is not the point (Dear ImGui for debug UI, VMA for GPU memory, MikkTSpace for tangents, texture encoders, DLSS and FSR). Cloth, rigid bodies, ragdolls and global illumination are built by hand: they are the point.
- Measure before optimising, and keep graphics features that cost a lot (ray tracing, GI quality) as settings that scale down to typical GPUs.

---

## Phase 0: Foundations (done, Sept 17-19 2026)

The original ten-step plan's steps 1-6, built on the CPU before any GPU code. Branch names are given so each step can be found in `git log`.

- [x] **1. Pixel buffer** (`pixel`, `pixelbuffer`, `image-output`)
  - Store, clear, set and retrieve pixels
  - Write the buffer to an image file to check the output
- [x] **2. Display and game loop**
  - SDL window, texture presentation, event handling
  - Configurable frame rate
- [x] **3. 2D rasterization** (`drawing-primitives`)
  - Lines, triangle outlines, filled triangles
  - Clipping, with tests
- [x] **4. Mathematics foundation** (`math-foundation`)
  - `Vec2`, `Vec3` and `Vec4`; vector arithmetic
  - Dot and cross products
  - Matrices (`Mat4`)
  - Transforms and coordinate spaces
- [x] **5. 3D transformations and camera** (`camera-3d-transformations`, `refactor-camera`)
  - Model, world, view and projection matrices
  - Perspective projection
  - Camera movement
  - Rotating wireframe cube
- [x] **6. 3D rasterization** (`3d-rasterization`, `scene-objects`)
  - Triangle projection
  - Back-face culling
  - Near-plane clipping
  - Barycentric coordinates
  - Depth buffer
  - Scene of multiple objects; flat shading (the software renderer is kept as a tested reference)

**Milestone:** a rotating, depth-tested 3D scene drawn entirely on the CPU. ✔

## Phase 1: GPU rendering and assets (done, Sept 19-27 2026)

Not in the original plan: the engine moved from the software renderer to the GPU here. Also covers most of the original step 7 (shading and lighting).

- [x] **GPU renderer** (`gpu-rendering`)
  - SDL_GPU with the Vulkan backend and SPIR-V shaders compiled by `glslc`
  - Depth texture, per-object uniforms
  - Fullscreen toggle (Alt+Enter)
- [x] **Shading and lighting, part of original step 7**
  - Surface normals, from the file or computed per face
  - Flat shading; smooth shading where the model provides per-vertex normals
  - One directional light with ambient
  - Materials: base colour and optional texture
  - Not done yet (moved to phase 4): point lights, several lights, specular
- [x] **Textures and materials** (`textures-materials`)
  - Texture upload and a texture cache
  - UV conventions: OBJ bottom-left, glTF top-left
- [x] **Model loading** (`gltf-loading`)
  - OBJ loader: polygons, normals, UVs
  - glTF/GLB loader (cgltf): node hierarchy, base colour, external and embedded textures
  - Model bounds and normalization to a target size

**Milestone:** textured glTF models drawn on the GPU. ✔

## Phase 2: Core loop and input (done, Sept 27 - Oct 8 2026)

Covers the input and "separate simulation and rendering" parts of the original step 10, and the fixed timestep from the original step 8.

- [x] **Control modes** (`camera-control-modes`)
  - First person, MOBA and free camera, switched with F1-F3
  - MOBA: confined cursor, edge panning, Space to lock the camera on the player
- [x] **Click-to-move** (`click-to-move`)
  - Ground plane and camera ray picking
  - Right-click (or hold) to move, destination marker, smooth turning
  - Reusable `Character` and `CharacterConfig`
  - `Input` snapshot: held, pressed and released keys; accumulated mouse motion
- [x] **Fixed timestep** (`fixed-timestep`)
  - 120 Hz simulation ticks with an accumulator and a 0.25 s frame time clamp
  - Render interpolation between ticks, with shortest-path angles
  - Input handled once per frame and passed to the simulation as commands
- [x] **Engine `Key` enum** (`engine-key-enum`)
  - `Input` no longer depends on SDL; translation lives in `sdl_input`

**Milestone:** a duck you can steer by clicking, moving smoothly at any frame rate. ✔

### Where the rest of the original plan went

| Original step | Now |
|---|---|
| 7. Shading and lighting: point lights | Phase 4 (rendering upgrades) |
| 8. Particle physics: forces, gravity, integration | Phase 5 (collision and movement) |
| 9. Rigid-body physics: orientation, angular velocity, mass and inertia, collision | Collision in phase 5; rigid bodies and joints in phase 8 (ragdolls); stacking and box contacts in phase 14 |
| 10. Engine architecture: scene management, entities and components, resource loading | Phase 3 |
| 10. Engine architecture: debug tools | Debug drawing in phase 4; profiler in phase 5; debug UI in phase 9 |

## Phase 3: Engine architecture (done, Oct 8 2026)

Untangle `Application` and the old `GpuDisplay` before adding more features, so everything after this has a clear place to go.

- [x] Entity/component layer (`entity-component`): entities with generations, sparse-set component storage, `World::each` systems; `Transform`, `PreviousTransform`, `ModelRenderer`, `Spinner` and `CharacterMovement` components replace `Scene`, `MeshInstance` and `ModelInstance`
- [x] Split `GpuDisplay` (`platform-split`) into `Window`, `GpuRenderer` and a richer `Input` (mouse buttons with exact click position and timestamp, cursor, window size); ground clicks, steering and edge panning moved into game code
- [x] `Game` interface (`game-interface`): `onInit`, `onInput`, `onFixedUpdate`, `onUpdate`; `Application` is the engine loop and the demo is `DemoGame`
- [x] Camera split (`camera-split`): `Camera` is a lens (position, orientation, FOV, aspect) with matrices built on request; free-fly, first-person and MOBA are separate concrete controllers, with look angles stored as yaw/pitch
- [x] Asset manager with handles (`asset-manager`): `MeshHandle` and `TextureHandle`, each file loaded once, GPU copies indexed like the handles, no per-frame path lookups
- [x] Indexed meshes (`indexed-meshes`): `Mesh` stays the import format; `buildIndexedMesh` produces the render format (`MeshVertex` position/normal/UV, the exact GPU layout, plus 32-bit indices), merging identical corners

**Milestone:** the current demo runs as a `Game`, and a test runs its simulation for 1000 ticks with no window. ✔ (`tests/demo_game_test.cpp`; the remaining items finish the phase)

## Phase 4: Rendering upgrades (done, Oct 8-9 2026)

Finishes the old "shading and lighting" step and adds the tools the next phases need to see what they are doing.

- [x] Light data in a uniform buffer instead of hardcoded in the shader; several directional and point lights (`lights-uniform-buffer`)
- [x] Specular highlights (Blinn-Phong) (`specular-highlights`)
- [x] Spotlights (`spotlights`): cone with inner and outer angles, up to 4; the demo gets a night scene and an F-toggled flashlight (at the camera, or carried by the duck in MOBA mode)
- [x] Emissive materials (`moonlight`): surfaces that glow with their own light; the demo gets a faint moonlight and a glowing moon sphere you can fly to in free camera mode, and its MOBA marker glows instead of using the temporary unlit flag
- [x] Optional smooth normals for models that have none (`smooth-normals`): opt-in per load, angle-weighted, grouped by position, with a crease angle
- [x] Normal matrix computed once per object on the CPU; move the depth range correction into the renderer (`cpu-normal-matrix`)
- [x] Linear filtering and mipmaps (`texture-filtering`): trilinear plus 16x anisotropic, mip chains generated on the GPU at upload
- [x] Gamma-correct colour (`srgb-colour`): image files store sRGB (gamma-encoded) values, but textures are created as `UNORM`, so lighting, blending and mipmap averaging treat them as linear light and come out slightly too dark. Use `R8G8B8A8_UNORM_SRGB` textures and an sRGB swapchain so all maths happens in linear light; data that is not colour (future normal maps) stays `UNORM`. Material and light colours will need retuning, since the whole scene will look brighter
- [x] Shadow mapping
  - [x] Shadows from every light (`shadow-mapping`): one depth atlas; point lights take six tiles (cube faces), spotlights one perspective view, directional lights one texel-snapped orthographic box around the camera; normal offset plus slope bias, 3x3 PCF; `castsShadows` on lights and models (default on). The demo's moon was a shadow-casting point light inside the moon sphere for a while, then went back to a directional light (sharper shadows)
  - [x] Tiles sized by the light's reach (`light-priority`): 256, 512 or 1024 pixels for about 0.04 units per texel, the least important shrinking first when the atlas is full, packed as a quadtree; room for 16 shadowed point lights and 8 spotlights
  - [x] First give the renderer the whole frame (`frame-description`): the engine builds a frame description (every `{mesh, matrix, material}` to draw, plus lights and camera) and calls one `render(frame)`, instead of calling `drawMesh` per object. Shadows need it (the scene is drawn twice), frustum culling needs it, and it is the basis for a later ray-tracing backend
- [x] Debug drawing (`debug-drawing`): lines, boxes, spheres and capsules drawn on top of the scene; games add shapes in `onDebugDraw`, and the engine draws every mesh's bounding box when the game asks (demo: F4, with light markers and the walk target). Colliders join the view once phase 5 adds them
- [x] Dithering (`dithering`): the main shader adds up to half an 8-bit step of per-pixel noise (interleaved gradient noise) in sRGB before output, so slow dark fades (the flashlight's pool at night) no longer show rings; mirrored and tested in `core/dither`
- [x] Frustum culling (`frustum-culling`): six planes from any view-projection (`math/frustum`), a bounding sphere per draw from its mesh's bounds, the camera's view culls the main pass and each shadow tile culls with its own light view (never the camera's, so off-screen things still cast shadows); `RenderStats` counts drawn and skipped meshes
- [x] Light priority (`light-priority`): seats in the shader and shadows go to the lights that matter most instead of the first ones in component storage (which shuffles when components are removed). Lights whose range sphere is outside the view are skipped; the rest are ranked by brightness at a focus point the game names (the camera, or the MOBA camera's ground point), `intensity / (d^2 + r^2)` without the range window so far lights do not tie at zero; last frame's seats and shadows count 1.25 times as much (hysteresis), so similar lights do not swap every frame. Seats raised to 64 point and 8 spotlights. The demo gets a field of 42 lamps with pillars (L) and a light count line in the F4 readout
- [x] Light and shadow follow-ups, in this order (each is tested in the game before the next starts)
  - [x] 1. Manual priority (`light-manual-priority`)
    - `int priority = 0` on `PointLight` and `SpotLight`. A higher priority always wins a seat and a shadow over a lower one; importance and hysteresis decide only between equals
    - A game marks a boss's aura or a story lamp, and it is never dropped while in view
    - Changes only the ranking function, plus tests
  - [x] 2. Fading handovers (`light-fading`)
    - Each seated light has a strength between 0 and 1 for its light and another for its shadow. Strengths ramp over 0.25 s, and the shader scales by them
    - A light losing its seat or shadow keeps it while fading out, so the caps count fading lights too; the winner fades in afterwards
    - Lights just switched on or come into view appear at full strength (nothing showed them before); lights leaving the view go at once
    - Needs: `frameSeconds` in `FrameDescription`, strengths in `LightHistory`, and a shadow strength per slot in the shader (light strength is multiplied into the radiance on the CPU)
    - Comes before caching because it changes how slots are handed over
  - [x] 3. Give back tiles for cube faces that see nothing (`shadow-face-culling`)
    - A face whose view pyramid does not overlap the camera's view cannot shadow a visible pixel, so it gets no tile and no draw
    - `Frustum::mayIntersect`: the corners of each view against the other's sides, both ways round (conservative). A left-out face keeps its place in the tile list with an empty rectangle, so the shader's `firstTile + face` lookup is unchanged, and it counts as lit
    - In the demo's lamp field it mostly skips the face that points back towards the camera (about 16 of 96 faces); the F4 readout counts tiles drawn and skipped
    - Frees atlas room and draw time for caching to build on
  - [x] 4. Shadow caching (`shadow-caching`)
    - Tiles stay where they are between frames and are redrawn only when the light moves, the tile changes, or something that casts shadows moves inside its view
    - `ShadowAtlasLayout` (`render/gpu/shadow_atlas`): each tile keeps its square while its light (by entity) and face ask for the same size; a buddy allocator merges freed quarters back; if scattered free space cannot fit a new tile, everything is repacked once
    - `ShadowTileMemory` (`render/gpu/shadow_cache`): what was last drawn into each square (matrix plus a hash of the casters' meshes and model matrices); drawing a square forgets anything it overlaps
    - The atlas is loaded instead of cleared; a redrawn tile is first reset by a "draw depth 1" triangle (`shadow_clear.vert`); with nothing to redraw there is no shadow pass
    - In the demo's lamp field, standing still, about 49 of 82 tiles are cached; the rest see the spinning objects, the flashlight or the moon box. The F4 readout counts tiles drawn, cached and skipped
    - [x] Shadow cap raised from 16 to 64 point lights (`more-point-shadows`), as many as the shader's seats: the atlas still fits everything at the smallest tile size. `LightLimits` lets a caller cap shadows below the seats (the tests do, to keep shadow-only handovers covered). The shader now skips a light, and its 9 shadow lookups, for pixels beyond its range. All lamps in view in the demo cast shadows
    - [x] Sharper spotlight shadows (`sharper-spot-shadows`): spotlights may grow to 2048-pixel tiles (`largestShadowTileSize`; directional boxes and point faces stay at most `standardShadowTileSize`, 1024) and aim for 0.02 units per texel (`ShadowSettings::spotTexelTarget`) instead of 0.04: their beams land on the ground at a slant well short of their reach, which stretched each texel, and the floodlights' step and pole shadows were jagged. The floodlights now get 2048 and the flashlight 512 (it had 256). They are few and usually still, so the cached tiles cost nothing; when the atlas is full, point lights shrink to the smallest before any spotlight shrinks
  - [x] 5. Soft shadows (PCSS, `soft-shadows`)
    - The shader first searches the shadow map near the point to estimate how far away the blocker is, then widens its blur by `emitterRadius * (receiver - blocker) / blocker`: the penumbra geometry of a solar eclipse
    - A new `emitterRadius` on point lights (0.05) and spotlights (0.025; first 0.1 and 0.05, which looked too soft) instead of `sourceRadius`, which was tuned as a falloff softener far bigger than any real bulb (4.5 on the demo flashlight)
    - The atlas is bound a second time with a plain sampler for the blocker search; 16 + 16 Poisson-disc samples, rotated per pixel; receiver plane depth bias so wide filters do not shadow sloped surfaces; filter 1 to 12 texels, kept inside the tile
    - Directional lights keep the 3x3 filter: a real sun or moon's penumbra is smaller than a texel of the 30-unit box
- [x] Faster builds (`faster-builds`): a one-line change took about 76 s (27 s for the game, 49 s for the tests); now about 2 s, a clean build of everything about 10 s
  - [x] Compile each `.cpp` to its own object file in `build/obj/`, and only link at the end, so a change recompiles only what it touches
  - [x] Let GCC record which headers each file uses (`-MMD -MP`), so a header change recompiles exactly the files that include it
  - [x] Build in parallel on every core (`-j`), with output kept in order per file
  - [x] The game and the tests share the engine's object files, so the engine compiles once instead of twice
- [x] Low-latency presentation (`platform-split`): one frame in flight, configurable present mode, mailbox by default

**Milestone:** a lit scene with shadows, and a toggle that draws every collider and bounding box. ✔ (bounding boxes; colliders join the F4 view in phase 5)

## Phase 5: Collision and movement (done, Oct 9-18 2026)

What both genres actually need from physics: knowing what you hit and moving a character through a level without passing through walls.

- [x] Settings in one place (`settings-structs`), done before physics adds dozens of new constants: tuning values live in one settings struct per system, owned where the system lives, read every frame and editable through `getSettings()`: `EngineSettings` (tick rate, frame time clamp, frame cap, present mode, and `RenderSettings`: `LightPrioritySettings` for seats, shadow caps, hysteresis and fade time, `ShadowSettings` for texel target, directional distance and reach, normal offset), and a settings struct per camera controller (sharing `LookSettings` for mouse sensitivity and sprint). Merged on the way: the first-person and free cameras' shared look-and-move steps (`turnCamera`, `moveCamera`), and angle wrapping (`math/angles`). Numbers the shaders share with C++ (light array sizes, soft-shadow filter limits, the dither step) now come from macro-only headers both include (glslc gets `-Isrc` and dependency files). Physics settings (gravity, substeps, cloth stiffness) start out in the same shape
- [x] Shapes (`collision-shapes`): `Sphere`, `Capsule` (a segment grown by a radius) and `Aabb` (the old `ModelBounds`, moved to `math/`), a `Shape` variant, and exact overlap tests between every pair (touching counts), built on closest-point queries (`math/closest_points`: point-segment, point-box, segment-segment, segment-box)
- [x] Raycasts against shapes and against triangle meshes (`raycasts`): `raycast(ray, shape or mesh, maxDistance)` returns the first `RayHit` (distance in world units, point, unit normal facing back against the ray); a ray starting inside a shape hits at distance 0; triangles are hit from either side (Moller-Trumbore, every triangle for now). `TriangleMesh` (positions and index triples) is the collision mesh, built from a scene mesh with `collisionMesh(mesh, transform)`
- [x] Mouse picking (`mouse-picking`): click a unit or object, not only the ground. A `Collider` component (a `Shape` in the entity's own coordinates plus an optional shared `TriangleMesh` `detail`); `worldShape` moves it by the model matrix (radii by the largest scale, boxes to the box around the turned box, `sum |A_ij| h_j`); `raycastColliders(world, ray, RaycastDetail)` finds the nearest, in `Triangles` mode confirming each shape hit against the detail mesh, with the ray carried into the mesh's coordinates by `affineInverse` and the normal back by the normal matrix. Characters get a capsule; the demo's props get mesh colliders. In MOBA mode left-click selects (outlined in cyan); F4 draws every collider
- [x] Broad phase (`broad-phase`): uniform grid, so tests only check nearby objects. `UniformGrid` (`physics/uniform_grid`): cubes of `CollisionSettings::gridCellSize` (2) in a hash map of occupied cells (Teschner hash), items in every cell their box touches or on a large list past `largeItemCells` (64), mailboxing so each item is reported once, region queries, and ray walks near to far (Amanatides and Woo) that stop once the nearest hit is closer than the next cell. `CollisionScene` (`physics/collision_scene`) is rebuilt from scratch from every `Collider` (never stale; incremental updates would need change tracking the ECS lacks) and answers `raycast` and `overlapping`; `raycastColliders` builds one for a single query. Tested against brute force on random scenes. The demo outlines what the cursor is over in MOBA mode, and F4 shows colliders, colliders tested and cells walked for that ray
- [x] Static level collision against triangle meshes (`static-level`): `StaticLevel` (`physics/static_level`) merges the pieces that never move into one world-space `TriangleMesh` with its own `UniformGrid` over the triangles, built as pieces are added and never rebuilt (huge ground triangles go on the large list). `raycast(level, ray)` returns a `LevelHit` (hit plus triangle); `contacts(level, capsule)` returns a `Contact` (point, normal pointing out, depth, triangle) per touched triangle, from `closestPointsSegmentTriangle` (`closestPointOnTriangle` by Voronoi regions, Ericson 5.1.5). A core that pierces a triangle is pushed out along the face normal on its middle's side, by its deepest end plus the radius. `raycastTriangle` is shared by mesh and level raycasts, and its edge test allows a tiny margin so rays cannot slip through the seam between two triangles (found in the demo). Both match brute force on random levels. The demo gets an obstacle course (floor slab, wall with doorway, five 0.25 steps to a platform, a 20 degree and a 50 degree ramp) made of cube parts, whose collision mesh comes from the same parts; the ground joins the level. Hover picks the level when it is nearer than any collider (so walls hide what is behind them), and F4 outlines the level triangle under the cursor with its normal and shows a level readout line
- [x] Kinematic character controller: capsule, sliding along walls, stepping up stairs, ground snapping, gravity and jumping (first person has a camera-only placeholder jump until then, `fps-jump`). Tuned to feel like Overwatch (community-measured numbers: 5.5 m/s walking, 90% backwards, 3 m/s crouched, a 5.72 m/s jump, 17.5 m/s' gravity with quadratic drag to a 30 m/s terminal speed, slope-consistent speed and speed-keeping air steering from the 2018 movement update)
  - [x] The controller (`character-controller`): `CharacterController` component and `moveCharacter`/`moveCharacters` (`physics/character_controller`): move in substeps of at most half the radius, push out of level contacts deepest first (ground straight up by depth / n_y, walls along the normal removing the velocity into them, steep slopes sideways only while walking), edges of ledges up to `stepHeight` count as ground so the capsule rides up steps, settling onto the ground every tick in 0.05 drops (snapping, no hovering), semi-implicit gravity with drag, jumps only from the ground, ground and air acceleration. The MOBA duck walks through it (`updateCharacters` takes the level; `MoveToController` now gives a velocity), climbs the course's stairs and ramps, and right-clicks aim at the level. F4 shows its ground normal, slope and substeps
  - [x] First-person body (`first-person-body`): `FirstPersonBody` (`gameplay/first_person_body`), an invisible capsule entity with a `CharacterController`; once a frame WASD becomes a wished velocity turned by the camera's yaw and normalised (`firstPersonWishVelocity`: 5.5 m/s, 90% straight back, 93.3% on the back diagonals, 3 m/s crouched, 1.5 times sprinting with Shift), Space a jump on a fresh press, Ctrl a crouch (shorter capsule, eyes eased down over 0.1 s, standing only where the full capsule fits: `characterFits`); the camera sits at the body's interpolated eyes and `FirstPersonCameraController` only looks; F1 from the free camera drops the body where the camera is. The camera-only walking and placeholder jump are gone
- [x] Particle physics (`particle-physics`): a `Particle` component (`physics/particle`) for balls that fly and bounce off the level. Flight under constant gravity uses the average of the starting and ending velocities (`fly`), which is exact, so throws follow the true parabola; collisions are swept, not stepped: `sweepSphere` finds the first touch of a moving sphere against the level (the centre's path raycast against each triangle grown by the radius: two offset faces and three edge capsules), so nothing passes through a wall at any speed. Bounces use restitution and Coulomb friction as impulses (`bounceVelocity`), which also gives sliding the kinetic friction deceleration mu g; arriving slower than a resting speed does not bounce, so a ball settles and then rests exactly still. The demo throws grenades with G in first person (`gameplay/grenade`: 15 m/s along the view, gone after 5 s). The shared part for cloth and orbits is the per-particle maths; cloth keeps its own array store in phase 7, and the functions become templates over the vector type when phase 13 adds `DVec3`
- [x] Frame and system timing (`profiler`, moved here from the UI phase): `Profiler` (`core/profiler`) with numbered sections, `ScopedTimer` and `timeSection`, on `steady_clock`; it counts per call (ticks do not fall one per frame) and shows the average and worst call of each finished half-second window, so the text is readable and spikes are not averaged away. The F4 readout gets the engine's frame, work (without the frame cap's sleep), tick, update, scene and render times (render includes waiting for the GPU: SDL_GPU cannot time the GPU itself) and the demo's cost per tick of each system. Physics is the first work whose cost grows with the scene, so the switch to the release build below, and every later performance decision, should rest on measurements
- [x] `release` build target (`release-build`, moved here from the optional list): `make release`, `make run-release` and `make test-release` build with `-O2` into `build/release/` beside the unchanged `-O0` debug build, and the packaged demo ships the release exe. `Vec2`, `Vec3`, `Vec4`, `Mat4 * Vec4`, `transformPoint` and `transformDirection` moved into their headers so they inline. Measured with 40 characters walking a stepped level and 200 bouncing balls, per tick: debug 0.52 and 0.54 ms; `-O2` alone 0.105 and 0.090 ms (5-6 times faster); with the operators inlined 0.075 and 0.067 ms (7-8 times faster than debug); link-time optimisation (`-flto`) only 0.073 and 0.061 ms, for a one-file rebuild of 20 s instead of 3 s, so it is left out. The `-O2` test build also showed a false array-bounds warning inside `std::sort` in `closestPointsSegmentBox`, now an insertion sort of its at most 8 cuts
- [x] Phase 5 documentation (`collision-guide`, last item of the phase): a new "Collision and movement" chapter in `docs/ENGINE_GUIDE.md`, written for a reader with a maths and physics background who is new to engine programming, so it explains the software choices as carefully as the maths. Planned sections:
  - Shapes as cores plus radii: why spheres and capsules reduce to closest points between points and segments, the segment-segment and segment-box minimisations, and why touching counts as overlapping
  - Raycasts: the ball quadratic, the capsule side as a 2D circle problem, the slab method for boxes, Moller-Trumbore for triangles, rays that start inside, and why normals face back against the ray
  - Colliders: shapes in local coordinates, moving them into the world (largest scale for radii, `sum |A_ij| h_j` for turned boxes), and why games click simple shapes rather than meshes
  - Two-stage picking: shapes first, triangles to confirm, the lower-bound argument that makes skipping safe, carrying the ray into a mesh's coordinates with the affine inverse, and bringing normals back with the normal matrix
  - The broad phase grid, static level collision against triangle meshes, the kinematic character controller (sliding, stepping, ground snapping, jumping) and particle physics (semi-implicit Euler, bouncing), each as built
  - Measuring speed: the profiler, and why the `-O2` release build matters for physics
  - Programming notes met along the way: `std::optional`, `std::variant` and `std::visit`, `std::shared_ptr`, overloading, floating-point tolerances, and squared distances
  - Update the guide's "Covers" line, and check `README.md` and `CLAUDE.md` still match the code

**Milestone:** walk around a small level in first person with walls, stairs and jumping, and throw a grenade that bounces.

## Phase 6: Rendering foundations, lighting and global illumination ← current

Light that bounces: the biggest visual step up from today's direct-only lighting, and done early because it is one of the two things we most want to see. Everything here also serves later phases: the HDR target settles where the HUD goes, compute pipelines run the cloth solver, and physically based materials carry fabric shading.

**Re-planned on Oct 18, 2026** after a three-way review of the roadmap (a hyper-critical, a neutral and an advantages-focused reviewer, who then answered each other). The decisions:
- **SDL_GPU is replaced by the engine's own Vulkan 1.3 renderer before global illumination.** SDL_GPU never hands out its Vulkan device, so it cannot reach hardware ray tracing, GPU timestamps, bindless resources, GPU-written draw counts or DLSS/FSR, and the old phase 14 plan ("SDL keeps the window while a second backend talks to Vulkan") would have meant every pass written twice. Raw Vulkan (with volk, VMA and vk-bootstrap) rather than a layer such as NVRHI: full performance and the real concepts, with NVRHI as the fallback if the port stalls past about four weeks. SDL stays for the window, input and audio.
- **Work that does not touch the graphics API comes first (6a), renderer work after the port (6c onwards)**, so no renderer code is written twice.
- **DDGI is traced with hardware ray queries first**; a software tracer for GPUs without ray tracing follows as the lower quality setting (ray tracing stays optional).
- **Shader maths keeps its tested CPU mirrors**, and a GPU test tier checks that the real shaders match them.
- Gaps a modern renderer needs that were missing are added below (6c, 6e): cascaded sun shadows, temporal anti-aliasing, reflections, transparency, volumetric fog.

**Done so far**
- [x] HDR off-screen render target and tone mapping: render the scene to a high-precision off-screen texture, then a final pass rolls brightness above 1 off smoothly instead of clipping to white and writes the 8-bit screen image. The dither moves from `triangle.frag` into that final pass (the off-screen image needs none). It comes before the HUD (phase 9) because it changes where everything drawn on top of the scene goes, so the HUD is built once, over the finished image. Bloom and DDGI build on it. Built: an `R16G16B16A16_FLOAT` scene image, `tone_map.frag` over a full-screen triangle, four curves (Clamp, the old look; Reinhard; ACES; AgX, the default) with an exposure in `RenderSettings::toneMapping`, mirrored and tested in `render/gpu/tone_mapping`, and the debug lines drawn after tone mapping in their exact colours
- [x] HDR display output (`hdr-output`): on an HDR monitor with Windows HDR on, show light brighter than white as brighter on the panel, not just as white. Built: `render/gpu/display_output` (`DisplaySettings`: `hdrOutput`, and `paperWhiteNits` and `peakNits` as HDR games offer them, 0 = what Windows reports; `chooseDisplayOutput`), an scRGB swapchain, and the renderer reading the window's HDR properties every frame and swapping the swapchain and its two pipelines when HDR turns on or off
  - Once tone mapping exists, consider physical light units plus a camera exposure for all lights. Emissive surfaces and the lights they stand for could then be linked physically (the light a surface receives from the moon is the moon's brightness times the solid angle it covers), with the exposure step mapping the real dynamic range (the moon disc is 10,000+ times brighter than moonlit ground) to the screen. Until then, the demo keeps the moon sphere's glow and the moonlight as separate artistic dials
- [x] Bloom (`bloom`): the scene halved down a chain of up to 6 levels (Jimenez's 13-tap filter, the Karis average against fireflies on the first halving) and grown back with a 3x3 tent; tone mapping mixes `strength` (0.04) of the blur in place of the same share of the scene, so no light is added. `BloomSettings` in `RenderSettings`; mirrored and tested in `render/gpu/bloom`
- [x] Physically based materials (`pbr-materials`, glTF metallic-roughness): GGX, Smith height-correlated visibility and Schlick Fresnel, Lambert diffuse weighted by what the specular part leaves, and multiple-scattering energy compensation (Turquin 2019) from a specular albedo table integrated on the CPU at start-up, so a white surface passes the white furnace test at every roughness; ambient split into diffuse and specular so metals are not black; `metallic` and `roughness` on `Material`, plus a linear metallic-roughness texture (the engine's first non-sRGB texture), read from glTF with its emissive factor and strength; the BRDF multiplied by pi so lights keep their brightness. Mirrored and tested in `render/gpu/brdf`. Not yet: occlusion and emissive textures, reflections of the surroundings (6e)

**6a. Groundwork that does not touch the graphics API** (all of it carries over to the Vulkan renderer unchanged)
- [x] Engine hygiene (`engine-hygiene`): the GPU device's debug mode only in debug builds (it is hardcoded on, which skews every release measurement); a determinism test (the same commands give the same world hash after N ticks), so the headless, deterministic simulation stays that way before cloth and ragdolls make it fragile; a deferred create/destroy queue for `World::each`, so systems can spawn and remove entities mid-walk (`updateGrenades` does it by hand today). Built: `EngineSettings::gpuValidation` (default `isDebugBuild`, from `-DENGINE_DEBUG_BUILD` in the debug build), `hashSimulationState` (`engine/state_hash`) with the shared `Fnv1aHash` (`core/hash.h`, also used by the shadow cache) and a test playing the demo twice side by side, and deferred `destroy`/`remove` plus `addLater` in `World`
- [x] Quaternions (`quaternions`, moved here from phase 8): `math/quat` (multiply, rotate a vector, axis-angle, matrix, slerp, the log map for joint errors), and `Transform::rotation` switched from Euler angles to a `Quat`, with `interpolate` using slerp. A tumbling body passes through gimbal lock, and blending three angles separately does not give the in-between orientation; ragdolls and glTF skins need them. Earlier is cheaper: 29 uses in 10 files today. Built: `Quat` (header-only, stored x y z w as glTF; `fromAxisAngle`, `fromEulerAngles` in the old x-then-y-then-z order, `rotate`, `toMatrix`, `conjugate`, `normalized`, `toRotationVector` and `fromRotationVector` as the log and exp maps, `angleBetween`, and `slerp` with the short-way flip and a normalised linear blend under about 1.8 degrees); `Transform::getMatrix` built straight from the rotation matrix; spinners keep Euler angles and convert each tick; characters keep their own `CharacterMovement::yaw` (the turning logic unchanged) and write the rotation from it
- [x] Reversed-Z and camera-relative rendering (`reversed-z`, moved here from phase 13): the far plane at depth 0, where floating-point values are densest, so depth precision is nearly even over the whole view (an FPS's long sightlines, the solar system later); and positions made relative to the camera before they become floats in matrices. All GPU depth knowledge sits in `toGpuDepthRange`, which keeps it small. Built: `toGpuDepthRange` maps to near 1, far 0 for every GPU projection (camera, shadow faces, directional box), `GREATER` depth tests, clears and tile resets to `gpuFarDepth` (`render/gpu/depth_shared.h`, shared with the shaders), the shadow comparisons and `linearShadowDepth` flipped, and `ClipDepth::OneToZero` for frustums (two surfaces 1 cm apart at 1 km: about 270 floats between their depths, none before); `render/gpu/camera_relative` (`cameraRelativeViewProjection`, `relativeTo`) for the main pass, light positions and shadow lookup matrices measured from the camera (12 km out the screen error drops from about a quarter pixel to about 20,000 times less), with the shadow pass, culling and debug lines left in world space so cached shadow tiles survive camera movement. The far plane stays finite
- [x] Debug UI (`debug-ui`, moved here from phase 9): Dear ImGui, with a `visit()` per settings struct (name, value, range, unit), so one list of fields drives the sliders, quality presets (Low / Medium / High / RTX), saved settings and the tests of the defaults. Tuning GI, PBR and cloth without sliders is slow. Built: `SettingsVisitor` (`core/settings_visitor.h`: float, int and bool fields with hard limits, a slider range and a unit, enums as lists, groups, start-up fields marked; doubles and unsigned counts converted once in the base class) and a `visit()` next to every settings struct; `engine/settings_file` (capture, apply with clamping and warnings, `settings.cfg` holding only values changed from the defaults, loaded by `Application` before anything reads them); `DebugUi` (`engine/debug_ui`, F5: Dear ImGui 1.92.9b, vendored in `external/imgui`, one window built from the visitor, start-up fields greyed out, Save and Reset to defaults) with `Game::visitSettings` for the game's own settings; the UI drawn into its own 8-bit overlay image and laid over everything last by `overlay.frag` (`render/gpu/overlay_renderer`, the blend mirrored and tested in `overlay_composite`), decoded and scaled to the white level so it looks right in HDR, the path phase 9's HUD will take; quality presets Low, Medium and High (`render/gpu/quality_preset`; High = the defaults; the lower ones are first guesses until 6b's GPU timings), RTX to follow in 6d
- [x] Normal maps (`normal-maps`, moved here from phase 10): per-vertex tangents generated with MikkTSpace (the standard every baking tool uses, so maps look as their authors made them), `UNORM` normal textures read from glTF, and the shading normal bent per texel. Every glTF asset uses them, and PBR looks flat without them. Built: a `Vec4` tangent in `MeshVertex` (xyz along +u, w the bitangent's side), taken from glTF's `TANGENT` when every corner has one, else generated by vendored MikkTSpace (`scene/tangents`, compiled as C) with v counted up the picture (`Mesh::uvOrigin`: glTF's v is flipped first), checked against the tangents Blender exported for NormalTangentMirrorTest; `Material::normalTexture` (linear) and `normalScale` from glTF; the shader rebuilds the bitangent as w · (N × T) from the unnormalised interpolated frame, as MikkTSpace asks, with w times the model matrix's `handedness` so mirrored objects keep their bumps; scale 0 without a map, so the white stand-in changes nothing; shadow offsets keep the geometric normal. Mirrored and tested in `render/gpu/normal_mapping`. A gallery of three Khronos test models left of the path in the demo. Not yet: mipmapped normals shortening into extra roughness (Toksvig), with 6e's TAA

**6b. The Vulkan 1.3 renderer** (`vulkan-backend`)
- [x] Golden images first: render a few fixed demo views (the lamp field, the floodlights, a furnace sphere, HDR on and off) on SDL_GPU and keep them as references. Built (`golden-images`): six demo views (`start` and `start-hdr`, `gallery`, `lamp-field`, `floodlights`, `moon`; the gold teapot and the gallery stand in for a furnace sphere, which the night scene cannot light evenly) listed by `Game::goldenViews` and set up by `Game::prepareGoldenView`, each in a fresh game and engine with default settings (`SavedSettings::Ignore`), simulated to its time (`advanceToGoldenView`, shared with the tests that check every view is deterministic) and drawn off screen at 1280x720 for a fixed display (`goldenDisplayOutput`: HDR as SDR white at 200 nits, peak 800) by `GpuRenderer::capture`, which reads back the scene image (half floats) and the final image; `make golden-save` stores them in `tests/golden/<GPU>/` (float images packed losslessly into 8-bit PNGs twice as wide, about 6.5 MB for all twelve), `make golden` compares (`core/image_compare`: steps of 1/255 or half-float ulps, at most 1 step in at most 0.1% of pixels, a heatmap of the differences). Two runs, and the debug and release builds, match bit for bit
- [x] The port: volk (function loading), vk-bootstrap (device and swapchain set-up), VMA (memory), dynamic rendering and synchronization2, every pass moved over one for one (shadows, main, bloom, tone mapping, debug lines) and compared with the golden images, then SDL_GPU removed. Barriers written by hand per pass and explained in the guide; a render graph only later, if the passes outgrow that. Validation layers on in debug builds. Built (`vulkan-backend`): `VulkanRenderer` and its parts in `render/vulkan/` (volk, vk-bootstrap and VMA vendored in `external/`, MSYS2's validation layers found through `VK_ADD_LAYER_PATH`), running the same compiled shaders in SDL_GPU's descriptor layout, with SDL's push-style uniform blocks kept as dynamic offsets into a per-frame upload buffer (`FrameUniforms`, `UniformRing`) and every barrier written from named image states (`ImageState`, tested); built beside `GpuRenderer` behind a temporary interface until all twelve golden images matched SDL_GPU's bit for bit in the debug and release builds with the validation layers reporting nothing, then tried live (debug UI, HDR on and off, resizing, minimising, present modes), and SDL_GPU, the interface and ImGui's SDL_GPU backend removed. Explained in the guide's "The Vulkan renderer" section
- [x] GPU timestamps per pass and debug labels for RenderDoc and Nsight: the renderer timing breakdown (moved here from phase 14) in `RenderStats` and the F4 readout, so every later change is judged by its own number. Built (`gpu-timestamps`): `render/gpu/gpu_timing` (`GpuPass` in recording order, ticks to seconds with the counter's roll-over, tested), a timestamp query pool per frame in flight with a stamp before the first pass and after each (a skipped pass lasts 0, so the passes add up to the frame), read without waiting once the frame's fence passes, into `RenderStats::gpu`; the F4 readout averages them over the profiler's half second on a line of their own (`makeGpuProfiler`). `VK_EXT_debug_utils` on whenever available, in release too: every pass labelled and the main images named. First numbers on the RTX 5080 at the start view: 0.20 ms for the whole frame (main pass 0.11, bloom 0.04, shadows 0.03, tone mapping 0.02)
- [x] Frames in flight as an `EngineSettings` value: 2 by default (the CPU prepares the next frame while the GPU draws this one), 1 as a low-latency option. Built (`frames-in-flight`): `EngineSettings::framesInFlight` (1 to `maxFramesInFlight` = 2, `render/frames_in_flight.h`), switchable live from the debug UI: the renderer waits for the GPU, remakes each frame's own resources (command buffer, fence, semaphore, uniforms, descriptors, shadow tiles, debug line vertices, timestamps) and rebuilds the swapchain. Images only the GPU writes stay single, ordered by the existing barriers. Golden images unchanged and validation silent in both modes and across switches. At today's 0.2 ms of GPU work per frame the two modes measure the same; two pays off once the GPU's work grows towards the CPU's
- [x] A GPU test tier (`make gputest`, optional and separate from `make test`, which stays GPU-free): small compute dispatches run the real shader functions on fixed inputs and compare them with the CPU mirrors, plus the golden images. Built (`gpu-test-tier`): the shaders' maths moved into eight libraries of pure functions in `assets/shaders/` (`brdf.glsl`, `normal_mapping.glsl`, `lighting.glsl`, `soft_shadow.glsl`, `tone_mapping.glsl`, `bloom.glsl`, `dither.glsl`, `overlay.glsl`; no texture reads or pixel position inside, golden images unchanged bit for bit), included by the passes and by one compute shader per library in `tests/gpu/shaders/`; the engine's first compute pipeline (`ComputePipeline`, `render/vulkan/vulkan_compute`; `workgroupCount`) and a windowless `VulkanDevice` (vk-bootstrap's headless mode); `build/gputests.exe` (`GpuFunctionRunner`: storage buffers in and out, push constants, a compute-to-host barrier) comparing about 20,000 values in 32 groups with the C++ mirrors through `core/float_tolerance` (absolute plus relative, a tally of the worst share used), with a new C++ mirror for the falloff and spot cone (`render/gpu/light_falloff`). Tolerances: 1e-5 relative for arithmetic, 1e-4 with pow, exp2 and log2, exact for cube faces; sharp specular highlights get an allowance growing with GGX's condition number (4 delta / d), the noise a wrap-around 4e-3. First results on the RTX 5080: every group within 16% of its tolerance but the noise (40%); planted bugs failed at 4,000 to 27,000 times it. Validation silent, debug and release. Also fixed: the shared shader binaries' dependency files now live in one folder for both builds (a per-build folder let the other build keep stale shaders). Not done: the optional Slang trial (one source compiled to both SPIR-V and C++, which could replace the hand-written mirrors), left for later

**6c. GPU scene, compute and lights**
- [x] GPU scene (`gpu-scene`): lights, materials and per-instance data (matrices, material index, bounds) in storage buffers, which lifts the 64-light cap set by the 4 KB uniform window; bindless textures; draws sorted and instanced (moved here from phase 12), so the lamp field's 100 pillars cost a few draws instead of hundreds, in the main pass and in every shadow tile; and shadow casters culled against each light's range sphere before its faces (from phase 14's performance list). Built in three steps. First the same products in storage buffers (golden images unchanged bit for bit): `render/gpu/gpu_scene` (records, `MaterialTable` deduplicating materials per frame, `batchByMesh`), lights as std430 records carrying their own shadow (`GpuLights`, `attachShadows`; buffers for 1024 point and 256 spot lights, seats still 64 and 8 by default until clustered shading), `GpuMaterial` with bindless texture slots, `BindlessTextures` (16384 slots, update after bind) and `SceneLayout` (set 0 textures, set 1 the frame's scene, shared by the scene's pipelines), the passes binding once and drawing `vkCmdDrawIndexed(instanceCount, firstInstance)` per mesh; lamp field: 161 visible objects in 10 draw calls, CPU recording 1.75 to about 1.15 ms with every shadow tile redrawn. Then casters culled by the light's range sphere (`castersInReach`, `ShadowTile::reach`): 1861 shadow caster instances instead of 2299, 6 pixels of one golden image changed where a soft shadow's filter reached past a light's range. Then one world-space instance per object shared by every view, with a draw list of instance numbers and each view's matrix multiplied in by the vertex shader (the camera's from the frame block, a shadow tile's as a push constant): about 61 KB uploaded per frame instead of 155, golden references saved again for rounding (scattered soft shadow samples flipping; at most 6 steps of 255 in the final images). Bounds are not in the GPU records yet: nothing on the GPU reads them before GPU culling
- [x] Depth prepass (`depth-prepass`): depth first, then shading with an equal-depth test, so hidden pixels never pay for 64 lights and their soft shadows; shadow lookups skipped where the surface faces away from the light. Its depth also feeds clustering, SSAO, reflections and TAA. Built: not with the shadow pass's depth-only pipeline as first planned (it multiplies world matrices, the main pass camera-relative ones, which round differently, and `EQUAL` needs identical bits) but with the main pass's own `triangle.vert` and no fragment shader, `gl_Position` declared `invariant`; `render/gpu/depth_prepass` (`prepassDepthState`, `mainPassDepthState`: `GREATER` and written, then `EQUAL` and not written), its own rendering block storing depth for the main pass to load, `RenderSettings::depthPrepass` (on, switchable live), `GpuPass::DepthPrepass` (PREPASS in F4), and `facesLight` in `brdf.glsl` (mirrored and GPU-tested) asked before each light's shadow lookups. Golden images identical with the prepass on and off; `invariant` alone changed 10 references by rounding (single edge pixels). Measured on the RTX 5080 (main pass plus prepass, 2560x1440): floodlights 0.39 to 0.32 ms, start and gallery 3-4% less, the lamp field unchanged at 1.2 ms (little overdraw there: its cost is every pixel looping over 64 lights, which clustered shading removes); the prepass itself costs 0.005-0.017 ms
- [ ] Compute pipelines: the engine's first compute shaders, reading and writing storage buffers and textures, mirrored and tested on the CPU. One foundation shared by clustered lights, DDGI's probe updates, the GPU cloth solver (phase 7) and, if needed, Barnes-Hut gravity (phase 13)
- [ ] Clustered shading (moved here from phase 14): the view split into a 3D grid of clusters, a compute pass listing the lights that reach each, and each pixel looping only over its cluster's list, so hundreds or thousands of lights cost what a few do. Light priority then only hands out shadows, where its fades and hysteresis still pay off
- [ ] Cascaded sun shadows: four boxes of growing size from the camera outwards instead of one 30-unit box, so the sun casts shadows across a whole level and through the milestone room's window

**6d. Global illumination: DDGI** ("Dynamic Diffuse Global Illumination", Majercik et al. 2019, built in the form of its 2021 production follow-up, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"). Today light stops at the first surface it hits, so a room lit through one window is black wherever the sunbeam does not land, and the ambient light is a flat stand-in for all the light bouncing around. DDGI measures that bounced light at points in space each frame and lets every surface look it up. Research notes, exact formulas and the reference code's numbers: [docs/ILLUMINATION.md](docs/ILLUMINATION.md)
  - Needs first: the HDR target and tone mapping above (bounce light routinely adds up to more than 1 and would clip to white), compute pipelines, and the Vulkan renderer (for ray queries)
  - [ ] Probe grid: a 3D grid of light probes over the level, one every 1-2 m. Each stores the light arriving from every direction in a small octahedral texture (the sphere of directions unfolded onto a square, e.g. 8x8 texels), and all probes are packed into one atlas, like the shadow tiles. Diffuse light changes slowly across space, so a coarse grid captures it
  - [ ] Ray tracing with hardware ray queries: each probe's rays traced against acceleration structures of the scene (one per mesh, built at upload; moving objects refit), which report the exact hit, its material and whether it struck a back face. This is the shortest way to correct bounce light, and the reference any software tracer is checked against
  - [ ] Probe update in a compute shader: each probe traces about 64 rays per frame (the papers tried 32 to 256), in a spherical Fibonacci pattern given a new random rotation every frame so the samples add up to the whole sphere over time. Each hit records whether it struck a back face (no light, distance shortened, so nothing leaks through walls). Each hit gets direct light from the existing lights and shadow atlas, plus indirect light read from last frame's probes, so each frame adds one more bounce: infinite bounces for the price of one. Caveat: the shadow atlas only holds lights and cube faces the camera can see (light priority and face culling), so hits outside the view need lights kept for the probes, or shadow rays
  - [ ] Hysteresis blending: each texel keeps most of its old value and takes a little of the new one (the papers keep 85-98% per frame), which averages noisy rays into a smooth result over time, like a long-exposure photograph. Set as a time constant in seconds, not a fraction per frame, so it behaves the same at 60 and 240 frames per second; separate ones for light and for distances (kept steadier). Irradiance is stored as its fifth root so bright-to-dark changes do not lag, a texel whose new value differs a lot (a light switched on, a door opened) blends faster, and the game can signal big changes to speed up every probe for a moment
  - [ ] Leak prevention: each probe also stores the mean and mean-squared hit distance per direction (a second small octahedral texture, 16x16). When shading, a Chebyshev test on these (as in variance shadow maps) estimates whether the probe can actually see the point; a probe behind a wall gets almost no weight, so sunlight in one room does not bleed through into the next
  - [ ] Probe relocation and probe states: a grid ignores the level's shape, so some probes land inside geometry, where most rays hit back faces, or a hair from a surface. Over a few start-up iterations move each one (less than half a cell) out through the nearest back face or away from a too-close surface, against the static level only. Then give each a state: off (still buried, or sees no surface), vigilant (a static surface within about a cell: always updated, even off screen, because it carries bounce light), sleeping (nothing nearby: not traced) or awake (a sleeping probe inside a moving object's grown box, checked every frame). Sleeping saved 30-50% of the probe work in the 2021 paper
  - [ ] Sampling in `triangle.frag`: each pixel blends the 8 probes at the corners of the grid cell around it, weighted by distance (trilinear), visibility (the Chebyshev test, from a point moved off the surface by one tunable "self-shadow bias", mostly towards the viewer) and a soft test of whether each probe lies in front of the surface, then adds the result as diffuse light in place of the flat ambient
  - [ ] Debug view: draw each probe as a small sphere shaded with its stored light (and off, sleeping and vigilant probes marked by colour), so leaks, dark probes stuck in walls and slow updates are visible at a glance
  - [ ] Sky and atmosphere (Hillaire 2020): a physically based sky lit by the sun, so the moving sun of the milestone has a sky to match, and probes whose rays escape see real sky light
  - [ ] A software tracer for GPUs without ray tracing, as the lower quality setting: a coarse voxel grid of the scene built in compute (simpler than distance fields and handles moving objects), checked against the ray-traced result; probes only need blurry diffuse light, so coarse geometry is enough
  - [ ] Cascades for large worlds: a dense grid near the camera and coarser ones further out (each about twice the spacing of the last). The dense grid is a tracking window: when the camera moves a cell, the plane of probes behind it is reused in front (a circular buffer, so nothing is copied) and only that plane starts from scratch. Each volume fades out across its outer cell, sampled densest first, with the camera window's fade region centred on the camera so a jumping plane does not pop

**6e. Image quality**
- [ ] Motion vectors and temporal anti-aliasing (TAA): each pixel's movement since last frame, from `PreviousTransform` (already kept for interpolation) and last frame's camera, and a jittered projection; TAA averages over frames, which also lets soft shadows, DDGI sampling and screen-space effects use far fewer samples. Then DLSS and FSR as options (they need the Vulkan renderer's native handles)
- [ ] Reflections: parallax-corrected reflection probes first, then screen-space reflections, so metals and wet surfaces reflect their surroundings instead of the uniform-sky stand-in; ray-traced reflections later as an RT setting (phase 14)
- [ ] Ambient occlusion (GTAO) from the prepass's depth, for the small creases DDGI's coarse grid cannot resolve
- [ ] Alpha test and transparency: glTF's `alphaMode` (mask for foliage and fences, blend for glass) and a sorted transparent pass after the opaque scene
- [ ] Volumetric fog on the cluster grid: light scattering in the air, so beams and lamps light the fog around them

**Milestone:** a room lit only through a window by the moving sun fills with soft bounce light that changes as the sun moves, a red wall tints the floor beside it, and no light leaks through the walls into the next room; the moon and lamps glow with bloom; the lamp field runs with hundreds of lights; edges are smooth (TAA) and metals reflect the room.

## Phase 7: Cloth simulation

The physics half of the dressmaking game, early because it is the other thing we most want to see: fabric that drapes, folds and piles up without passing through itself. Builds on phase 5's particle store and collision shapes and phase 6's compute pipelines. The CPU solver does not depend on the renderer, so it can start earlier (any time after quaternions) if we want cloth sooner. Cloth code lives in its own module on top of the core, like the FPS and MOBA code. Garments, sewing and the game itself follow in phase 10, once phase 9 has skinned characters to dress.

- [ ] Job system (moved to the start of the phase): a work-stealing thread pool with `parallelFor` first, for simulation, frame building, culling and asset decoding. The cloth solver is the first work that needs more than one core
- [ ] Dynamic meshes: a GPU upload path for meshes whose vertices change every frame, with normals recomputed each time (today a mesh is uploaded once and never changes)
- [ ] Particle store for cloth: the sheet's vertices as plain arrays (positions, previous positions, velocities, inverse masses, with 0 for pinned points), contiguous for the cache and for upload to the GPU, inside the cloth component, reusing phase 5's per-particle functions (`fly`, `bounceVelocity`)
- [ ] XPBD solver (extended position-based dynamics): particles integrated with gravity, then constraints project their positions back towards valid states. Each constraint has a compliance (inverse stiffness), so nearly unstretchable fabric stays stable at the 120 Hz tick, where stiff springs would need a tiny timestep. Use many substeps (10-20 per tick, one constraint iteration each) rather than many iterations of one big step: small steps converge faster for the same work, so fabric stretches less (Macklin et al. 2019, "Small Steps in Physics Simulation")
- [ ] Cloth constraints: stretch (edge lengths), bending (angle between neighbouring triangles), pinning (fixed points, for testing and for hangers)
- [ ] Collision with bodies: spheres and capsules first (phase 5's shapes), then a signed distance field around a mannequin, built in compute (it can serve later as DDGI's software tracer and for distance-field AO); friction between cloth and body
- [ ] Self-collision, so folds and layers never pass through each other. A broad phase (a BVH or spatial hash) only finds nearby pairs; cloth is so thin that a fast-moving layer can jump right through another between two substeps, so it also needs continuous collision detection (vertex-triangle and edge-edge pairs checked along their motion during the step) and a cloth thickness that keeps layers apart
- [ ] Untangling pass (Baraff, Witkin & Kass 2003, "Untangling Cloth") as a safety net: finds intersections that slipped through anyway and pushes the tangled regions apart over the next steps, instead of letting them lock together for good
- [ ] Solver on the GPU (phase 6's compute pipelines) or on several threads (the job system above), once a sheet is too slow on one core, checked against the CPU solver
- [ ] Fabric properties: different stiffness along the threads (warp and weft) than across them (the bias), bending stiffness (silk vs denim), mass per area, friction; a few presets
- [ ] Optional, later: IPC or codimensional IPC (incremental potential contact) for hero garments: guaranteed intersection-free at any step size, at a much higher cost per step

**Milestone:** a sheet pinned at two corners drapes over a sphere; a sheet dropped onto itself folds into a pile with no intersections (the self-collision stress test); a silk sheet and a denim sheet fall visibly differently.

## Phase 8: Active ragdolls

Euphoria-style characters: a simulated body held up by joint motors and kept standing by balance controllers, so a shove makes it stagger, step, fall with its hands out and get up again. Layers 1-5 of the plan in `docs/RAGDOLLPLAN.md` (read it first: it holds the papers' formulas and numbers and how each layer fits the engine); layer 6, following animation clips, comes in phase 9 with skeletal animation. Placed straight after cloth because the rigid-body solver is phase 7's XPBD with rotation and inertia added; quaternions come from phase 6a. Real-world values throughout, as in the papers: gravity 9.81 for ragdolls (the character controller keeps its Overwatch 17.5), SI masses, torques and push forces, so the papers' results can be reproduced. Until phase 9 the body is drawn as its capsules, a mannequin.

- [ ] Rigid bodies (`rigid-bodies`, moved here from phase 14's rigid-body dynamics): a `RigidBody` component (mass, body-frame inertia, velocity, angular velocity, a compound of capsules) and an XPBD substep solver (Müller et al. 2020; 10 substeps per tick, 1.2 kHz), with gyroscopic torque, capsule contacts against the static level from `contact(capsule, triangle)`, restitution 0 and friction 0.8. Capsules only: there is no oriented box. `Mesh::capsule` to draw them. Tests: momentum and angular momentum conserved in flight, the tennis-racket theorem's tumble, a dropped capsule rests without jitter, and the same run twice is bit-identical
- [ ] Joints (`joints`): ball and hinge `Joint`s with swing-twist limits and soft limits, capsule-against-capsule self-collision between unjoined parts, and the ragdoll builder from `RagdollBodySettings` (1.8 m and 70 kg by default, anthropometric proportions, feet as two parallel capsules for a flat sole). Each part is an entity with a capsule `Collider`, so picking finds ragdolls with no new code
- [ ] Joint drives (`joint-drives`): an inertia-scaled PD servo per joint, stiffness as a natural frequency and damping as a damping ratio (GTA V's units), applied as equal and opposite torques on the two bodies, capped, and scaled by a global strength and health
- [ ] Gravity compensation (`gravity-compensation`): feed-forward torques from virtual forces (τ = Jᵀ F) that hold the body up, so the drives stay soft; faded out when the centre of mass leaves the feet's support area, so a falling body goes limp
- [ ] Standing balance and stepping (`standing-balance`, `stepping`): the torso held upright in world space with the stance hip taking the balance of the torques (SIMBICON), foot placement from the inverted-pendulum capture point (Coros 2010), two-bone IK, a step state machine, and failure detection. Headless test: a 300 N push for 0.1 s at the chest leaves it standing 3 s later
- [ ] Behaviours and supervisor (`falling`, `behaviour-supervisor`): behaviours that score the situation and write targets for the body parts they own (balance, stagger, catch fall, arms windmill, head look, get up), run by a supervisor; handover between a character's kinematic capsule and its ragdoll and back
- [ ] Debug view and demo: R spawns a ragdoll in front of the camera, E shoves what the view ray hits; F4 draws the capsules, joint frames, centre of mass, support area, next foot target and the active behaviours

**Milestone:** shove a capsule mannequin off the obstacle course's steps: it staggers, takes recovery steps, falls with its hands out and gets back up.

## Phase 9: Animation, UI and audio

- [ ] Parent/child entity hierarchy: attach entities to others and to skeleton joints (a weapon to a hand, a hat to a head, a turret to a vehicle). Skeletal animation is the first thing that needs it, and phase 12's first-person weapon uses it
- [ ] Skeletal animation from glTF skins: joints, playing and blending clips, and skinning in a compute shader once per frame into a vertex buffer that the main pass, every shadow view, ray tracing and cloth collision all read (skinning in the vertex shader would repeat the work in every shadow tile)
- [ ] Ragdolls follow animation (layer 6 of `docs/RAGDOLLPLAN.md`): a table from skeleton bones to ragdoll bodies, clip poses as the joint drives' targets (a powered ragdoll), skinned characters drawn from the bodies, softening and shaped impulses on a hit, then a smooth blend back into a matching clip or get-up animation
- [ ] Text rendering with a bitmap font (debug overlays already have a stroke font, `debugText`, good for numbers but not for real UI)
- [ ] HUD: crosshair, health bars above units, ability cooldowns. Start with flat-coloured screen-space shapes drawn after phase 6's tone mapping (no depth test, alpha blending) and a tested `worldToScreen(camera, point, windowSize)`; turn the demo's MOBA destination marker (an emissive plane for now) into a HUD ring of constant pixel size
- [ ] In-game stats in the debug UI (phase 6a): entity count, and phase 5's frame, tick and per-system timings
- [ ] Key bindings: games read actions (`MoveForward`, `LockCamera`) instead of keys, and players can rebind them
- [ ] Audio with SDL3: play sounds, volume, simple 3D panning by distance and direction

**Milestone:** an animated character that walks and idles with something held in its hand, with a HUD, sounds and a debug overlay.

## Phase 10: Garments and a dressmaking game

A third kind of game, on top of phase 7's cloth: cut fabric panels, sew them into garments and see them hang and move on a body. Needs phase 9's skinned characters and phase 6's physically based materials.

**Garments**
- [ ] Sewing patterns: 2D panel outlines triangulated into cloth meshes, with UVs from the flat layout
- [ ] Seams: constraints that pull matching panel edges together until they join, starting from panels placed around the body
- [ ] Dressing an animated character: the cloth collides with the skinned body as it moves

**Rendering**
- [ ] Fabric shading: a sheen term for the soft rim highlight of cloth, on top of physically based materials
- [ ] Weave detail with phase 6a's normal maps

**Game**
- [ ] Pattern editor: draw and edit panel outlines, choose which edges are sewn together
- [ ] Fabric choice, colours and prints per panel

**Milestone:** a simple skirt sewn from two panels hangs on a mannequin and swings as it turns; then a garment follows an animated character as it walks.

## Phase 11: Networking

Builds on the fixed tick and the headless simulation. Commands go up, snapshots come down. Deferred until there is a playable FPS slice (phase 12) worth playing together; the deterministic, headless simulation keeps it possible, and phase 6a's determinism test keeps it that way.

- [ ] Serialization of commands and snapshots into compact byte buffers
- [ ] UDP sockets, connections, and a small reliability layer for messages that must arrive
- [ ] Dedicated server build that runs the simulation headless
- [ ] Authoritative server: clients send commands, the server simulates and sends snapshots
- [ ] Snapshot interpolation for other players' entities
- [ ] Client-side prediction and reconciliation for the local player
- [ ] Lag compensation: the server rewinds hitboxes to what the shooter saw (FPS)
- [ ] Simulated latency and packet loss for testing

**Milestone:** two clients move around the same level through a dedicated server, with smooth movement under 100 ms of simulated latency.

## Phase 12: Genre demos

**Engine**
- [ ] Asset cooking (before large scenes): an offline step in `tools/` that turns source models and images into ready-to-load files: textures compressed to BC7 (colour) and BC5 (normal maps), about a quarter of the memory; meshes in vertex-cache order with LODs (meshoptimizer); tangents precomputed
- [ ] Level of detail: simpler meshes for distant objects, chosen per view (instancing itself moved to phase 6c)

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread. Projectiles and grenades (phase 5's particles) must also hit moving things, characters and props, not only the static level: sweep the sphere against the moving colliders' shapes too (a sphere or capsule is a raycast against the shape grown by the radius, like `sweepSphere`; a box needs a rounded-box sweep)
- [ ] Health, damage, death and respawn
- [ ] First-person weapon model (attached with phase 9's hierarchy) and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 13: Orbital simulation (a solar system)

A fourth kind of world: the Sun, planets and moons at real scale, moving under gravity. Like cloth, it lives in its own module on top of the core. It needs phase 5's particles and release build but no collision, animation or networking, so it can move earlier whenever we want it sooner.

**Precision and rendering**
- [ ] Double-precision vectors (`DVec3`) for simulation positions. A float keeps about 7 significant digits, so at Neptune's distance (4.5 billion km) positions would snap in steps of hundreds of kilometres; a double's 16 digits give about a millimetre. Float `Vec3` stays for rendering and everything else
- [ ] Camera-relative rendering in double (floating origin): phase 6a makes rendering camera-relative in float; here the subtraction happens in double before the small difference becomes a float, so the GPU only ever sees numbers near zero and far-away objects do not jitter
- [ ] Depth at planetary scale: phase 6a's reversed-Z, plus a logarithmic depth or split near and far passes if a nearby spacecraft and a planet millions of kilometres behind it still do not both sort correctly

**Simulation**
- [ ] N-body gravity with a symplectic integrator (leapfrog / velocity Verlet). Ordinary integrators slowly pump energy into or out of an orbit, so planets spiral outwards or fall into the Sun; symplectic ones keep the energy error bounded forever. Tested: total energy stays within a small band over many orbits
- [ ] Its own simulation clock with time warp: many substeps per 120 Hz tick so a year can pass in seconds, with the step size adapting to how close bodies come to each other. A naively varied step breaks the symplectic guarantee and the energy drifts again, so the adaptivity has to keep it (for example a time-symmetric step choice, or fixed-step substepping only for the bodies in a close pass)
- [ ] "On rails" Keplerian orbits for distant bodies: the position comes from the exact ellipse at any time, with no integration error and almost no cost. A body switches to full simulation when the player comes near (and back when they leave), so a spacecraft's thrust or a close pass can change its path
- [ ] Barnes-Hut (or the fast multipole method) for thousands of bodies, such as an asteroid belt: a distant cluster pulls like a single mass at its centre, as a far galaxy does, turning the O(n²) pair sum into O(n log n). Drawn with phase 6c's instancing

**Milestone:** the Sun, planets and Moon orbit stably for 100 simulated years at high time warp, and you can fly the free camera from Earth to Neptune without jitter.

## Phase 14: Later and optional

Ideas with no fixed place and nothing waiting on them: pick one up when a game needs it.

- [ ] More rigid-body dynamics, extending phase 8's solver (bodies, joints and capsule contacts come with the ragdolls): oriented boxes with box contacts (separating axes), stacking and piles of crates. This includes particles colliding with each other (grenades knocking each other about), left out of phase 5 because piles need this phase's iterative contact solver: find pairs with the uniform grid over each particle's swept box, detect the touch by sweeping in relative motion (a ray against a sphere of the summed radii, so fast crossings are not missed), and respond by running the relative velocity through `bounceVelocity` and sharing the change by mass (an equal and opposite impulse, so momentum is conserved), with the two materials' restitution and friction averaged
- [ ] Reuse per-frame memory: the frame's draw list, the shadow tile lists and the debug lines are rebuilt in fresh vectors every frame; keep and clear them instead. The demo rebuilds its `CollisionScene` on every `pick` (hover and a click can both pick in one frame); rebuild it once per frame. Minor each, so only if phase 6b's timings point at them (the renderer timing breakdown, cheaper caster culling and instancing that used to be listed here moved to phases 6b and 6c)
- [ ] Shadow redraw budget: at most N shadow tiles drawn per frame, most important first, far lights refreshed less often, so switching on many lights at once or turning quickly does not cost one long frame
- [ ] Separate shadow caches for still and moving casters: keep a copy of each tile with only the still world, and draw the moving things over a copy of it each frame, so one spinning object no longer forces whole tiles to redraw
- [ ] More hardware ray tracing in the Vulkan renderer (phase 6b; DDGI's probe rays come first, in 6d): ray-traced shadows (exact soft shadows from area lights), ray-traced reflections, then ReSTIR-style sampling of many lights or a path-traced reference mode. Ray tracing is always optional: settings in `RenderSettings` (so the debug UI and a player's options menu can reach them) switch each ray-traced feature on or off while the game runs, falling back to shadow maps, reflection probes and the software-traced probes. Off by default and greyed out on GPUs without ray tracing (detected at start-up); the game is designed, tuned and tested on the non-ray-traced path first, so it looks right without it
- [ ] GPU-driven rendering beyond phase 6c: culling in compute against each view and last frame's depth (Hi-Z occlusion culling), writing indirect draws with a GPU-written count; meshlets (meshoptimizer) and mesh shaders where the GPU has them; a visibility buffer if scenes ever get dense enough to need it
- [ ] Open-world scale: streaming textures and meshes in and out by distance, impostors for far objects, and a render graph that orders passes and places barriers from what each pass reads and writes, once the hand-placed barriers of phase 6b no longer keep up
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [ ] Level editor or scene file format
- [ ] API reference with Doxygen: a `make docs` target generating HTML into `build/docs/`, with Graphviz include and dependency diagrams (they show the source layering). Needs function comments switched from `/* */` to `/** */` so Doxygen picks them up. Complements `docs/ENGINE_GUIDE.md`, which explains concepts rather than the API
- [ ] Scripting for gameplay code
