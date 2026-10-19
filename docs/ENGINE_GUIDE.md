# Engine guide

How the engine works and why it is built this way. This is for people learning the engine, so it explains concepts as well as code. For the exact API, read the headers; for what comes next, see [ROADMAP.md](../ROADMAP.md).

This guide describes the engine as it is now. It is updated at the end of each roadmap phase and after any large feature.

**Covers:** phases 0 to 5, several games on one engine (the game registry, shared controls by composition, a settings section per game), the sun's place in the sky (declination, hour angle, latitude) and phase 6's physically based materials, 6c's GPU scene (storage buffers and std430, bindless textures and nonuniform indexing, instancing and the draw list, culling shadow casters by a light's reach, why changing the order of a product changes pixels), depth prepass (overdraw, early depth testing, equal-depth shading and invariant positions), clustered shading (exponential depth slices, conservative sphere and cone tests, one bit per light in slot order and why that keeps every pixel the same, shared memory and workgroup barriers, buffer barriers, waves and subgroupOr, the heatmap) the sun's disc in the sky (no parallax, radiance from illuminance and solid angle, the half-float cap), and cascaded sun shadows (practical splits, bounding spheres and texel snapping, choosing by box and blending, bias along the light, the sun's angular size and its penumbra, the Ultra preset and the grown atlas, fitting them to the screen's depths with a reduction and a ladder of steps), hardware ray tracing (ray queries, two-level acceleration structures and compaction, front and back faces, instance masks, device addresses, grazing hits and their tolerance), global illumination with light probes (irradiance, octahedral images, Fibonacci rays and hysteresis as a time constant, infinite bounces as a series, trilinear, back-face and Chebyshev visibility weights, the self-shadow bias, the specular stand-in, the volume's edge, specialization constants, the units), so all of 6c, all of 6a (engine hygiene, quaternions, reversed-Z and camera-relative rendering, the debug UI, normal maps) and all of 6b (the Vulkan renderer: volk, vk-bootstrap and VMA, command buffers, fences and semaphores, uniforms as dynamic offsets, validation layers, barriers and image states, GPU timestamps and debug labels, frames in flight, compute shaders; golden images and the GPU test tier: shader libraries of pure functions, float tolerances and condition numbers; normal maps: tangent frames, handedness and MikkTSpace; quaternions and slerp; collision shapes, raycasts, colliders and picking, the broad phase grid, static level collision, the character controller, particles, the profiler and the release build; lights including spotlights, emissive materials and the moon, physically based materials (microfacets, Fresnel, metals, multiple-scattering energy compensation, the white furnace test), smooth normals, normal matrix, reversed-Z depth and camera-relative rendering, mipmaps, the debug UI and settings file (visitors, presets, the overlay), gamma-correct colour and dithering, the frame description, shadow mapping, debug drawing, frustum culling, light priority, shadow face skipping, shadow caching and soft shadows).

---

## 1. The big picture

The engine is meant to run two very different kinds of game, an FPS and a MOBA. So the core knows nothing about either: no weapons, no abilities, no "player". Genre-specific code sits on top, in `gameplay/` and in each game's own folder under `games/`.

The source is split into layers, and **a layer may only use the layers below it**:

```
app/            picks a game and starts the engine
games/          one folder per game (games/demo, games/lightlab)
engine/         the main loop and the Game interface
platform/  render/          the window and SDL input / the GPU renderer
gameplay/       movement, camera controllers
physics/        collision shapes, raycasts, the level, character controller, particles
scene/  assets/ meshes, materials, lights, camera / loading files
input/          keys and the input snapshot
ecs/  core/  math/          entities, small shared types, vectors and matrices
```

Why bother? Because each rule buys something concrete:

- **Games never include `render/`, `platform/` or SDL.** So a game's whole simulation runs without a window or GPU, which is how the tests run the demo for 1000 ticks. Later, a dedicated server for multiplayer will need exactly this.
- **`render/` only receives plain data** (meshes by handle, matrices, materials, lights). So the renderer could be swapped (for example for a ray-tracing backend) without touching game code.
- **`input/` has no SDL types.** Game code asks "is W held?" with an engine `Key`, never an SDL scancode.

At start-up, `main` makes a game and an `Application` (the engine). The engine owns the window, the renderer, the input snapshot, the `World` of entities and the `AssetManager`, and drives the game through a few hooks.

### Several games on one engine

There are two games so far: the **demo** (a night scene of lamps, floodlights and an obstacle course) and the **light lab** (a sunlit test ground built for judging shadows and, later, bounce light). `game.exe` runs the demo; `game.exe --game lightlab` runs the other. The second game is also a test of the first rule above: if the engine really knows nothing about the demo, a second game should need no engine changes, and it needed only a title and a settings section of its own.

**Choosing one: a registry.** `app/game_registry` keeps one table of `{name, function that makes the game}`. The command line, the golden image runner and the tests all read that table, so adding a game is one line rather than an `if` chain copied into three places. The table holds *how to make* a game, not a game, because the golden images need a brand-new game for every view (nothing one view switched on may leak into the next). Each entry is a plain function pointer, which captures nothing, so the whole table is a compile-time constant.

**Sharing controls: composition, not inheritance.** Both games want the same F1 first person, F2 MOBA and F3 free camera, the same body and the same tuning. One way would be a base class, `class FpsMobaGame : public Game`, that both inherit from. The trouble is that a base class makes every child carry all of its choices: the dressmaking game, which wants an orbit camera and no body, would inherit machinery it has to fight, and any change in the parent silently changes every child (the "fragile base class" problem). Instead, the controls are an object, `PlayerControls`, that a game *holds as a member* and forwards to. A game that wants them holds one; a game that wants something else holds something else. What a mode change means for the game's own things (the demo's duck forgets its orders) stays in that game. Most engines build gameplay this way, from parts a game puts together rather than a family tree of classes.

**Settings for each.** All games share one `settings.cfg`. Each game's values live under its own section, named after its title (`demo.grenade.throwSpeed`, `lightLab.sun.solarHours`), and when one game saves, the other games' lines are read back from the file and written out untouched, so neither can wipe the other's.

---

## 2. The frame loop and time

### Two clocks: simulation and rendering

A game has to do two different things over time:

1. **Simulate**: move characters, spin objects, resolve collisions.
2. **Render**: draw a picture, as often as the screen allows (up to 240 times a second here).

If the simulation stepped once per rendered frame with whatever time had passed, it would behave differently on fast and slow PCs. A physics analogy: integrating `x += v * dt` with a varying, sometimes large `dt` gives different trajectories, and with collisions it can even let objects tunnel through walls. Numerical integrators are most predictable with a **fixed step**.

So the engine runs the simulation in **fixed ticks of 1/120 s** and renders independently.

### The accumulator

`FixedTimestep` keeps a bank of unsimulated time:

```
each frame:
    accumulator += real time since last frame   (clamped to 0.25 s)
    while accumulator >= tick:
        run one tick
        accumulator -= tick
    alpha = accumulator / tick                   (0..1)
```

A slow frame runs several ticks to catch up; a fast frame may run none. The 0.25 s clamp stops a "spiral of death": after a long pause (say, dragging the window), the engine would otherwise try to simulate seconds of backlog at once, making the next frame slow too.

### Interpolation: why movement looks smooth

After the ticks, some time is left over (`alpha` of a tick). If we drew objects exactly where the last tick put them, motion would stutter: at 240 FPS with 120 ticks, every other frame would show the same position.

So entities that move continuously keep a `PreviousTransform` (their pose at the start of the last tick), and rendering blends: `drawn = previous + (current - previous) * alpha`. Rotations blend along the shortest turn between the two orientations (slerp, see "Orientation: quaternions" in section 7), so something rotating past 360 degrees does not spin backwards for a frame.

Things that teleport (the destination marker) deliberately have no `PreviousTransform`: blending would make them slide.

### Input becomes commands

Each frame runs in this order:

1. `game.onInput(world, input)`: once. Clicks and key presses become **commands** ("move to this point").
2. `onFixedUpdate(world, tickSeconds, simulationSeconds)`: zero or more ticks. It has **no `Input` parameter on purpose**.
3. `game.onUpdate(world, input, frameSeconds, alpha)`: once. The camera moves here, so mouse look is as smooth as the frame rate, not limited to 120 Hz.
4. Render with `alpha`.

Why keep input out of ticks? A click is an *event* that happens once per frame. If ticks read input directly, a frame with three ticks would see the same click three times, and a frame with zero ticks would miss it. Turning input into commands first means each click acts exactly once. It is also the shape networking needs later: clients send commands, the server simulates.

---

## 3. Input

`Input` is a **snapshot** of one frame, with no SDL types in it:

- Keys: `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased`, using the engine's own `Key` (physical US-layout positions, so WASD is in the same place on any keyboard layout).
- Mouse buttons work like keys. Each press is also recorded with its **exact position and timestamp**, so a fast click is placed where it happened, not where the cursor is by the end of the frame.
- Accumulated mouse motion (for mouse look), the cursor position, window size, focus, and whether the mouse is captured or confined.

`platform/sdl_input` is the only place that translates SDL events into this. `Window::processEvents` fills the snapshot each frame and reports raw facts only. Rules like "Escape releases the mouse" live in game code.

---

## 4. Entities and components

### The idea

Instead of a class hierarchy (`Player : Character : GameObject`), the engine uses **entities and components**:

- An **entity** is just an ID: "thing number 7".
- **Components** are plain data structs attached to entities: `Transform`, `ModelRenderer`, `PointLight`, `CharacterMovement`...
- **Systems** are plain functions that run over every entity with a given set of components.

The flashlight is an entity with a `Transform` and a `SpotLight`. A spinning cube is `Transform` + `ModelRenderer` + `Spinner`. New kinds of object come from new *combinations*, not new classes.

### Entities that go stale safely

An `Entity` is an **index plus a generation**. When an entity is destroyed, its slot can be reused, but the generation goes up. An old handle still has the old generation, so `world.isAlive(oldHandle)` is false instead of silently pointing at a different object. `Entity{}` is never alive and means "none".

### Sparse sets: how components are stored

Each component type has a `ComponentStorage<T>`, a **sparse set**:

- `components`: a packed array of all `T`s, with no gaps, so systems walk memory in order (fast for the CPU cache).
- `entities`: which entity owns each packed element.
- `sparse`: for each entity index, where its component sits in the packed array.

Adding, removing and lookup are all constant time. Removing swaps the last element into the hole, which keeps the array packed.

Systems run with `world.each<A, B>(function)`. It walks the first type's packed array and skips entities that lack the others, so list the **rarest component first**. Removing a component moves the last one into the hole, and adding one can move the whole array, so neither may happen to an array while `each` walks it. The `World` therefore counts the walks running (they can nest): inside one, `destroy` and `remove` are written into a queue and carried out, in order, when the outermost walk ends, much as a physics step computes every force before moving anything, so no update sees a half-updated world. A system can destroy what it is walking without a second loop. Adding a component of a type being walked throws instead (a deferred add could not hand back the component); `addLater` queues it.

---

## 5. Assets and handles

The `AssetManager` owns everything loaded from files: meshes, images and glTF models.

- **Each file is loaded once.** Asking for the same path again returns the same handle. The cache key includes options that change the result (a texture's vertical flip, a mesh's smoothing settings).
- **Handles, not pointers.** A `MeshHandle` or `TextureHandle` is a typed index. A default handle means "none", for example no texture means plain colour.
- **Assets are append-only.** Nothing is ever unloaded, so handles never go stale, and "what's new since last frame" is simply "everything past the renderer's count". The engine uploads new meshes and textures to the GPU before each frame, so games can load at any time.

The renderer keeps its GPU copies in arrays indexed exactly like the handles, so finding the GPU mesh for a handle is a single array access.

---

## 6. Meshes and normals

### Two formats

- **`Mesh`** is the *import* format: positions plus triangles, where each triangle corner may carry a normal and a UV (texture coordinate). Loaders and the software renderer use it.
- **`IndexedMesh`** is the *render* format: a list of unique vertices (position, normal, UV and tangent, exactly the GPU's layout) and 32-bit indices into it. Corners that are identical in every component are merged, so shared vertices are stored and processed once.

### What a normal is

A **normal** is the direction a surface faces, and lighting is computed from it. A file may give a normal for each corner. If it does not, the corner gets its triangle's **face normal** (from the cross product of two edges), and the model looks faceted: every triangle has one flat shade.

### Smooth normals (opt-in)

Curved models without normals, like the teapot, look blocky. `generateSmoothNormals(mesh, creaseAngle)` estimates the curved surface the triangles approximate:

- For each corner, average the normals of the faces around that **position** (by position, not vertex index, because some files duplicate vertices along seams; the teapot has 319 such duplicates).
- Weight each face by its **angle at that corner**, so the result depends on the shape of the surface, not on how it was cut into triangles.
- Only count faces within the **crease angle** (default 60 degrees) of the corner's own face. Gently curved areas get averaged; real edges (a cube's corners, a teapot's rim) stay sharp.

It is **opt-in** per load (`MeshLoadOptions{.smoothNormals = true}`), because only the game knows whether a shape is meant to be faceted: a stealth fighter's panels meet at shallow angles but should stay flat. Normals from the file are never replaced.

### Normal maps: bumps without geometry

Every lighting term depends on the normal, and between vertices the normal is only interpolated, so a surface with few triangles looks like smooth plastic. A **normal map** is a texture whose texels are small tilts of the normal. The light then behaves as if the surface had grooves, weave or scratches, though the geometry stays flat: silhouettes do not change, and nothing shadows itself. In physics terms it perturbs the boundary condition, not the boundary.

**Storing a direction in colour.** A texel holds three numbers c from 0 to 1, read as the direction n = 2c − 1. An untouched surface is (0, 0, 1), stored as (0.5, 0.5, 1): the light blue every normal map has. The texture is read as linear numbers, never decoded as sRGB (that would turn 0.5 into about 0.21 and tilt every flat texel). glTF's `scale` multiplies the sideways parts, so 0 flattens the bumps and 2 exaggerates them.

**The frame the picture is written in.** The direction is not a world direction; it is measured in a frame stuck to the surface: the **tangent** T (the way the texture's u increases), the **bitangent** B (the way up the picture) and the normal N. The world normal is n_x T + n_y B + n_z N, a change of basis like writing a vector in a sphere's local unit vectors instead of x, y, z. Because the frame travels with the surface, one brick picture works on walls, floors and barrels alike. T and B are the partial derivatives of position with respect to u and v, the columns of the UV map's Jacobian; on one triangle they follow from its two edges and their UV differences by inverting a 2x2 system.

**Handedness.** B is nearly redundant: it is ±(N × T). So each vertex stores T and a sign w (`MeshVertex::tangent`, a `Vec4`), and the shader rebuilds B = w (N × T). The sign is −1 where an artist mirrored part of the picture to paint it once for both sides of a face. Corners on a mirror seam agree in position, normal and UV but not in sign, so they stay separate vertices. A mirrored object (a model matrix with a negative determinant) flips the rebuilt bitangent too, since (Ma) × (Mb) = det(M) M⁻ᵀ(a × b); the vertex shader multiplies w by the matrix's `handedness` so its bumps stay bumps.

**Why MikkTSpace.** Each vertex needs one frame, but its triangles each have their own, and averaging them involves choices: weights, where to split, whether the order of triangles matters. Bakers once all chose differently, and a normal map is only right in the exact frame it was baked in (like components of a vector read in the wrong basis), so mismatches showed as seams and odd shading. Morten Mikkelsen's MikkTSpace fixed one order-independent answer that Blender, Substance, Unity and Unreal now share, and glTF asks for it when a file has no tangents. The engine uses the file's tangents when it has them and otherwise runs the vendored MikkTSpace (`scene/tangents`). MikkTSpace's bitangent follows increasing v, and glTF counts v down from the top of the image, so glTF UVs are handed over as (u, 1 − v) (`Mesh::uvOrigin`). A test regenerates the tangents Blender exported for a Khronos test model and gets the same ones, signs included. MikkTSpace also asks the shader to use the interpolated normal and tangent as they arrive, unnormalised, and to normalise only the result, because that is what its baker does.

**Odds and ends.** A material without a normal map gets scale 0 and the white stand-in texture, which gives exactly the old normal, so the shader needs no branch. Shadow lookups keep the geometric normal: tilts far smaller than a shadow texel would only add noise there. The demo has a gallery of three Khronos test models left of the path: CompareNormal (the same shape without and with a map) and two grids of bumps whose pictures are turned and mirrored cell by cell. Every cell should look lit from the same side; a wrong sign or a flipped green channel shows up as cells lit from below. Not yet: a distant mipmap averages tilted normals into a shorter one, a sign of bumps too fine to see, which should become extra roughness (Toksvig's correction), planned with TAA.

---

## 7. Cameras

### A lens, and separate controllers

`Camera` is just a lens: position, forward and up directions, vertical field of view, aspect ratio, and near and far planes. It builds its matrices on request, so changing the field of view (aim-down-sights zoom) needs no extra work. It has no movement logic.

Controllers in `gameplay/camera/` move it:

- `FirstPersonCameraController`: mouse look only. Walking, jumping and crouching belong to an invisible first-person body with a character controller (a capsule that collides with the level), and the game puts the camera at the body's eyes each frame.
- `FreeFlyCameraController`: mouse look plus flying (Space and Ctrl for up and down). Holding Shift doubles the flying speed; the first-person body has its own sprint (1.5 times walking).
- `MobaCameraController`: a fixed angle that follows a target, or edge-pans while the cursor is confined.

Look controllers store direction as **yaw and pitch** (spherical coordinates: yaw 0 looks along -Z, positive yaw turns right, pitch is clamped to 89 degrees so the view never flips over the pole).

### The matrices

Points travel through a chain of coordinate systems, each step a 4x4 matrix:

```
model space  --model-->  world space  --view-->  camera space  --projection-->  clip space
```

- The **model** matrix places an object (its `Transform`).
- The **view** matrix moves the world so the camera sits at the origin looking down -Z.
- The **projection** matrix applies perspective: after the GPU divides by `w`, distant things shrink.

Convention: `Mat4` is row-major (`values[row][column]`) and multiplies column vectors (`M * v`), in right-handed coordinates.

### Orientation: quaternions

A `Transform` stores its rotation as a **unit quaternion** (`Quat`, `math/quat`), not as three angles. Three Euler angles (turn about x, then y, then z) are easy to read but have two faults. **Gimbal lock**: when the middle turn reaches 90 degrees, the first and last axes line up, so two angles do the same thing and one direction of turning is lost; near that pose a small change of orientation needs a huge change of angles, and a tumbling body passes through it all the time. **Blending**: blending each angle separately does not follow the real in-between orientation, except for turns about one axis.

A quaternion is a scalar plus a vector, q = (w, **v**), the way a complex number is a real part plus an imaginary one. (Hamilton, who invented them in 1843, coined the words *scalar* and *vector* for these two parts.) Their product is

  (w₁, **v**₁)(w₂, **v**₂) = (w₁w₂ − **v**₁·**v**₂, w₁**v**₂ + w₂**v**₁ + **v**₁×**v**₂).

The cross product in it is why the order matters, just as two turns of a book give different results in different orders. A turn by θ about a unit axis **n** is q = (cos θ/2, **n** sin θ/2), and it rotates a vector as **p**′ = q **p** q*, with q* = (w, −**v**) the conjugate undoing the turn. The 2D version is a unit complex number e^{iθ} turning the plane by multiplication; in 3D the sandwich is needed so the result stays a pure vector, and each side carries half the angle.

The half angle has a consequence a physicist will know from spin-½: q and −q are the same rotation. Unit quaternions live on a 4D sphere that covers the rotations twice (it is SU(2) covering SO(3)), and a full 2π turn stores −1.

What the engine does with them:

- **Composing**: `a * b` turns by `b` first, then `a`, the same order as matrices multiplying column vectors. `toMatrix` reads the rotation matrix straight from the components, with no trigonometry.
- **Blending** (`slerp`): the shortest arc between two orientations on that sphere, at constant angular speed, so a drawn frame between two ticks is the true in-between orientation. Because of the double cover, `to` is flipped to −`to` when the two are on opposite sides, which makes the turn take the short way round. Below about 1.8 degrees apart, a plain blend normalised back to unit length is used instead, which avoids dividing by the sine of a tiny angle and is just as accurate there. At 120 Hz that is the common case.
- **Errors as vectors** (`toRotationVector`, the log map): any turn as one vector, axis times angle. `(target * current.conjugate()).toRotationVector()` is what is left to turn, and which way, to reach a target. It can drive a spring as a displacement would, which the ragdolls' joint drives (phase 8) will use. `fromRotationVector` (the exp map) goes back, and turns an angular velocity times a time step into the turn made in that step.
- **Angles where they are natural**: mouse look keeps a yaw and a pitch, and a character keeps its own `yaw`, since "turn at most this fast towards that heading" is easiest in one angle. A spinner keeps three angles and their speeds. Each builds its quaternion at the end (`fromAxisAngle`, `fromEulerAngles`), and nothing reads angles back out of one.

---

## 8. Drawing a frame on the GPU

The renderer is the engine's own, written directly against **Vulkan 1.3** (`render/vulkan/`). Shaders are written in GLSL and compiled to SPIR-V by `make`. Until phase 6b the engine drew through SDL_GPU, SDL3's portable GPU API; why it changed, and what talking to Vulkan directly involves, is explained below under "The Vulkan renderer".

### Each frame: describe it, then hand it over

The engine first writes the whole frame down as plain data, a `FrameDescription`, and then gives it to the renderer in one call:

```
frame = buildFrame(world, camera, alpha)   a copy of the camera, every light, and one DrawItem
                                           {mesh, model matrix, material, castsShadows} per visible model part
renderer.render(frame)                     the renderer decides how to draw it
```

`buildFrame` reads the `World` (`ModelRenderer` + `Transform`, interpolated by `alpha`, and the lights through `collectLighting`). It contains no GPU code, so tests can check exactly what would be drawn.

Why not just let the engine call "draw this mesh" for each object, as it used to? Because some techniques need to see the whole scene before drawing any of it. Shadows draw the scene twice, first from the light and then from the camera; culling skips what the camera cannot see; a ray tracer needs every object up front. A list of everything to draw allows all of these, while a stream of "draw this now" calls allows none. Inside, `render` acquires the next screen image, draws the shadow map if a light casts shadows, draws the camera's view, and presents.

The renderer works on its own copy of the camera, because only it knows the size of the image it got from the window, and so the aspect ratio. Afterwards the engine copies that ratio into the game's camera too, so turning mouse clicks into rays matches what is on screen.

### Uniforms, storage buffers and why their layout is strict

Small data that is the same for every vertex or pixel of a draw goes to shaders as **uniforms**. The renderer *pushes* bytes into a numbered slot, and the shader reads them as a struct. Pushed data stays in effect for the rest of the frame. (Vulkan itself has no "push into a slot"; how the renderer builds it is described under "The Vulkan renderer".) The scene itself, every object, material and light, no longer goes this way but into storage buffers, read as arrays (see "The GPU scene" below); uniforms remain for small per-pass settings such as tone mapping, bloom and the frame's camera.

The shader reads those bytes with **std140** layout rules, and C++ does not know them. The classic trap: a `vec3` takes 16 bytes in std140, not 12, and every element of an array is padded to 16 bytes, so an array of 64 floats takes 1 KB. To make mismatches impossible, every block uses only 4-component vectors, and each is mirrored by a C++ struct with a `static_assert` on its size (`GpuFrameData`, 128 bytes; `ToneMapUniformData`). Storage buffers follow **std430**, which packs arrays tightly (a float takes 4 bytes, a `vec4` 16, like C++), and their records are mirrored and size-checked the same way.

Uniform blocks are also small by design: the renderer lets a shader see only the first 4 KB of each one (the window SDL_GPU used, kept so the port changed nothing), and a shader reading past it gets garbage with no error anywhere. (The engine learned this the hard way, back on SDL_GPU: when the shadow tile data grew past 4 KB, whole cube faces of shadow suddenly read as solid black.) Bigger data goes in a **storage buffer**, a plain GPU buffer the shader reads like an array, with no such limit; the per-tile shadow data (each tile's matrix and place in the atlas, 112 bytes per tile, up to 396 tiles, about 44 KB) lives in one, uploaded once per frame. Every uniform struct has a `static_assert` that it stays under 4 KB. If anyone changes one side, the build fails.

Matrices are transposed on the way, because GLSL stores them column by column.

### The Vulkan renderer: talking to the GPU directly

For its first phases the engine drew through **SDL_GPU**, a portable layer that SDL puts over Vulkan, Direct3D and Metal. It was a good teacher, but it keeps the real Vulkan device to itself, so nothing outside it can use what Vulkan offers beyond the common subset: hardware ray tracing (which phase 6d's global illumination traces with), GPU timers, bindless textures, draws whose count the GPU writes itself. So phase 6b replaced it with the engine's own renderer, written against Vulkan 1.3. SDL still opens the window and reads the keyboard and mouse; it just no longer draws.

What changes when you talk to Vulkan yourself is how much you must say out loud. A layer like SDL_GPU guesses a sensible answer to a hundred questions; Vulkan asks every one of them. Three libraries take the tedious parts, and are vendored in `external/`:

- **volk** fetches Vulkan's functions from the graphics driver when the program starts, so the engine does not link against any Vulkan library at build time.
- **vk-bootstrap** does the start-up ritual: create the instance, pick the GPU (a dedicated one if there is one), check it has the features the renderer needs, make the device and its queue, and make the swapchain (the ring of screen images).
- **VMA** (the Vulkan Memory Allocator) finds memory for each buffer and image. A GPU has several kinds: fast memory only the GPU sees, memory the CPU can write into for uploads, and memory the CPU reads quickly for downloads; VMA picks the right kind and packs many objects into each big allocation.

The CPU and the GPU are two workers running at the same time. The CPU does not draw; it **records** a list of commands (begin this pass, bind that pipeline, draw this mesh) into a command buffer, then **submits** the list to the GPU's queue and moves on. The GPU works through it later, at its own pace. Two kinds of signal keep them in step: a **fence** lets the CPU wait until the GPU has finished a submission (before it reuses that frame's buffers), and a **semaphore** makes one piece of GPU work wait for another (drawing waits until the screen image has been handed over; presenting waits until drawing is done). How far the CPU may run ahead is the number of **frames in flight**. With one, the CPU records frame N + 1 only once the GPU has finished frame N: they take turns, a frame costs the CPU's time plus the GPU's, and the delay between a mouse movement and the screen is as short as it gets. With two (the default), the CPU records frame N + 1 while the GPU still draws frame N, like two cooks where one preps the next dish while the other plates this one: a frame costs only the slower of the two, for up to one frame more delay. The rule that makes this safe: anything the CPU writes for a frame (its command buffer, uniforms, shadow tile list, debug line vertices, timestamps) has one copy per frame in flight, so the CPU never overwrites what the GPU is still reading, and each frame's fence says when its copies are free again. Images only the GPU writes (the scene, the shadow atlas, the bloom chain) need only one copy, because the GPU works through submissions in order and the barriers (next section) also order one frame's use of an image after the last frame's. "Frames in flight" in the debug UI switches between one and two while the game runs (the renderer waits for the GPU, remakes each frame's parts and rebuilds the swapchain), so the F4 numbers can be compared directly; today the GPU needs 0.2 ms a frame, finishing long before the CPU has recorded the next, so two only starts to pay off once the GPU's work grows towards the CPU's.

Two Vulkan 1.3 features make the code much shorter than older Vulkan: **dynamic rendering** (a pass is begun by naming its images on the spot, instead of building "render pass" and "framebuffer" objects in advance) and **synchronization2** (a clearer way of writing barriers, the subject of the next section).

**Uniforms.** Vulkan has no "push bytes into slot 3". What it has are buffers, and descriptors that point a shader's slot at a piece of a buffer. The renderer rebuilds SDL_GPU's convenient model on top (`FrameUniforms`): each frame gets one 4 MB upload buffer, every push copies its block to the next free spot, and when a draw is recorded, each slot's descriptor is pointed at its latest block by an offset (a "dynamic offset", so no new descriptor is made per draw). It is the same idea as a stack of paper forms: never erase a form the GPU may still be reading, just write on the next sheet and tell the GPU which sheet to look at.

**The pictures did not change.** The port was done one pass at a time, with SDL_GPU still running beside it, and checked against golden images (see Testing): the twelve reference images taken on SDL_GPU before the port came out of the Vulkan renderer bit for bit identical, in both the debug and the release build. That only works if every small choice SDL made is copied, and some are surprising: SDL flips the viewport upside down (so y runs down the window, as the shaders and matrices assume), counts counter-clockwise triangles as front-facing, and clamps depth rather than clipping it. Each is kept on purpose and commented where it is set. Only then was SDL_GPU removed.

**The validation layers** are Vulkan's referee. Vulkan does not check what you ask of it, for speed: a mistake is undefined behaviour, which may work on one driver and corrupt the picture on another. In debug builds the renderer loads the validation layers, which check every call against the specification and report any breach. They must report nothing. They slow every call down, so the release build skips them unless the "GPU validation" setting is switched on.

### Barriers: telling the GPU what waits for what

This is the part of Vulkan that SDL_GPU did silently, and the one most worth understanding.

A GPU is not a calculator that finishes one command before starting the next. It is a long production line of **stages** (fetch vertices, run vertex shaders, rasterise, run fragment shaders, write colours, test depth), and many draws and passes move through it at once, overlapping. It also has **caches**: a pass that writes an image may leave the newest values in a cache close to the stage that wrote them, not yet in memory where the next stage would look. So "draw the shadow atlas, then read it in the main pass" is, by default, *not* an order the GPU promises to keep. Nothing stops the main pass from reading the atlas while the shadow pass is still writing it, or reading stale values from memory while the new ones sit in a cache.

A **barrier** is the instruction that restores the order. It says three things:

1. **Execution: who waits for whom.** "Work in these stages after the barrier must not start until work in those stages before it has finished." Physicists can think of it as forcing a causal order between two events that would otherwise be spacelike-separated: without it, neither is guaranteed to come first.
2. **Memory: make the writes visible.** "Flush these kinds of writes out of their caches, and make them visible to those kinds of reads." Waiting alone is not enough; the reader must also see the data.
3. **Layout: how the image is arranged.** A GPU stores an image differently depending on its use (a depth target may be kept compressed; an image being sampled, or copied, or shown on screen, wants another arrangement), so the barrier also changes the image's **layout**. Moving to a new layout from "undefined" tells the GPU the old contents may be thrown away, which is cheaper than keeping them.

Getting it wrong is the classic Vulkan bug: too few barriers and the picture flickers or tears in ways that differ between GPUs; too many and the GPU idles, waiting for work it did not need to wait for.

**How the engine writes them.** Barriers in raw Vulkan are a dense structure of stage and access flags, easy to get subtly wrong. The engine names the handful of ways it ever uses an image instead (`ImageState`, in `render/vulkan/image_state`):

| State | Meaning |
|---|---|
| `Undefined` | just made; contents meaningless |
| `Acquired` | a screen image just handed over by the swapchain |
| `ColourTarget` | being drawn into as a colour image |
| `DepthTarget` | being drawn into as a depth image |
| `FragmentRead` | read by fragment shaders (a colour texture) |
| `DepthFragmentRead` | a depth image read by fragment shaders (the shadow atlas) |
| `TransferSource`, `TransferDestination` | copied from, copied into |
| `Present` | handed back to be shown on screen |

and one tested function turns "from state A to state B" into the barrier: wait for A's stages, flush A's writes (only writes: two reads in a row need no barrier at all, so the same read-only state twice records nothing), make the data visible to every access of B, and change to B's layout. Each image remembers its current state, and the state carries over from one frame to the next, so a pass that writes an image this frame still waits for last frame's reading of it to finish (the "write after read" hazard, the easiest to forget). The code that draws only ever says what it is about to do with an image, and the barrier follows.

**One frame's barriers**, in order:

- **Shadow atlas:** `DepthFragmentRead` (last frame's main pass sampled it) to `DepthTarget`, *keeping* its contents, because the shadow cache keeps unchanged tiles from earlier frames. The shadow pass redraws the changed tiles, then the atlas goes back to `DepthFragmentRead` for the main pass.
- **Scene image and depth image:** to `ColourTarget` and `DepthTarget`, *discarding* their contents, because the main pass clears them anyway. After the pass, the scene image goes to `FragmentRead` for bloom and tone mapping.
- **Bloom levels:** each level becomes a `ColourTarget` while it is drawn and a `FragmentRead` while the next step reads it. On the way down a level is overwritten, so its old contents are discarded; on the way back up the blur is *added onto* the level, so its contents are kept.
- **Debug UI overlay:** `ColourTarget` (discarded, then cleared to transparent) while the UI is drawn into it, then `FragmentRead` so the final pass can lay it over the picture.
- **Screen image:** `Acquired` to `ColourTarget` (discarded: tone mapping writes every pixel), one pass of tone mapping, debug lines and overlay, then `ColourTarget` to `Present`.

The screen image needs one more piece of care. The swapchain hands it over with a semaphore, not a barrier, and the frame's submission waits on that semaphore at the colour-writing stage. The `Acquired` state therefore starts its barrier from that same stage, so the barrier is chained after the semaphore: the layout change cannot happen before the image is really free. At the other end, `Present` needs no stage at all, because the semaphore that presentation waits for already covers everything before it.

Captures for golden images add one last pair: the scene and final images to `TransferSource`, copied into a buffer the CPU can read, and a barrier making that copy visible to the CPU before it reads the bytes.

### Timing the GPU: timestamps

The CPU's stopwatch (the profiler, in the collision chapter) cannot time the GPU's work. Recording a frame takes the CPU a fraction of a millisecond; the GPU does the drawing later, on its own. A clock around `render()` measures only how long it took to write the list of instructions, plus any wait for the previous frame. So "rendering is slow" could never say which pass was slow.

Vulkan's answer is a **timestamp query**: a command in the list that says "when you get here, write down your own clock". The GPU writes its tick counter into a small **query pool** when it reaches that point, and the CPU reads the numbers back later. Two things turn ticks into time. The device reports a **tick length** (`timestampPeriod`, nanoseconds per tick, often exactly 1), and the queue reports how many bits of the counter are real (`timestampValidBits`). Past its top value the counter starts again at zero, like a car's odometer, so differences are taken modulo 2 to the power of that number of bits. With 36 bits at 1 ns that is about a minute per turn, far longer than a frame.

The renderer writes one stamp before its first pass and one after each, so consecutive stamps bound every pass and the passes add up exactly to the whole frame. A pass that drew nothing this frame (no shadow tile changed, the debug UI closed) still gets its stamps, at the same moment, and so lasts zero. Each stamp is written at the "all commands" stage, meaning once everything recorded before it has finished. The GPU overlaps neighbouring work, so a pass's number really means "how much later the frame finished because of this pass", which is the number worth optimising.

The stamps can be read only once the GPU has finished the frame, and the frame's fence already says when. So each frame reads the stamps its own resources held last time round, just after waiting on that fence, without waiting any longer. The numbers shown are therefore a frame or two old, which is invisible at hundreds of frames a second. Single-frame GPU times jitter by 10 to 20 percent, so they go through the same profiler as the CPU's numbers: averaged over half a second, on a line of their own in the F4 view.

The same tool has a second use. **Debug labels** (from the `VK_EXT_debug_utils` extension) name stretches of the command list ("SHADOW", "BLOOM") and give images names ("Scene colour", "Shadow atlas"). RenderDoc, a frame debugger, and Nsight, NVIDIA's profiler, then show a frame as named passes instead of a flat list of thousands of draws and anonymous handles. With no such tool attached the driver ignores them, so they stay on in the release build too, which is the one worth profiling.

### The GPU scene: a catalogue instead of envelopes

Until phase 6c every draw handed the GPU its own envelope: the object's matrices and material pushed as uniforms, then a fresh descriptor set naming its six textures, then one draw call, and the shadow pass did the same for every caster in every shadow tile. The lamp field, whose lamps fly round their pillars so their shadow tiles are redrawn every frame, made about 2,500 envelopes a frame. Each one costs the CPU a little (recording commands, checking state, writing descriptors), and the GPU, which works through big batches far faster than through many small ones, was left waiting on that bookkeeping.

The **GPU scene** sends a catalogue instead. Once per frame the renderer writes every object (its **instance**: world matrix, normal matrix, which way it is handed, the number of its material), every different material and every light into **storage buffers**, and every texture lives in one array that is set up once. A draw then says only "draw mesh M, for entries 40 to 149 of the draw list". The shaders look up everything else themselves: the vertex shader finds its object from its instance number, the fragment shader its material from the number the object carries, and its textures from the numbers the material carries. Nothing is bound per object.

**Instancing.** With nothing per object left to bind, the only thing that still differs between two draws is the mesh. So the renderer sorts the visible objects by mesh, and each run of the same mesh becomes one instanced draw: `vkCmdDrawIndexed(indices, instanceCount, ..., firstInstance)` draws the mesh `instanceCount` times, and inside the shader `gl_InstanceIndex` counts from `firstInstance`. That number indexes the **draw list**, a list of instance numbers: first the camera's visible objects in batch order, then each redrawn shadow tile's casters. In the lamp field the 161 visible objects are 10 draws, and 1861 shadow casters are 783 draws. The sort is stable (equal meshes keep their order), so the same frame always gives the same list.

**One record, many views.** Each object's instance holds its world matrix, so the camera and every shadow tile share it: each view multiplies in its own matrix on the GPU. The camera's goes in the frame's uniform block, and a shadow tile's is a **push constant**, 64 bytes written straight into the command list before the tile's draws. The vertex shader keeps camera-relative rendering exact: it subtracts the camera from the matrix's translation before multiplying anything, the same subtraction `relativeTo` does on the CPU (a test checks they agree bit for bit), so far from the origin the small numbers stay small.

**Bindless textures.** A descriptor is the GPU's handle to a resource (an address, a size, a format), and shaders normally see them through small sets bound per draw. With **descriptor indexing** (Vulkan 1.2) a shader can instead declare an array with no fixed size, `sampler2D textures[]`, and pick an element by a number it read from a buffer. The renderer keeps one set of 16,384 slots: slot 0 is a white texture (a missing texture multiplies by 1), and each uploaded texture takes its asset handle plus one. Two details matter:
- **`nonuniformEXT`.** A GPU shades pixels in lockstep groups (a warp on NVIDIA: 32 at once, one instruction for all). Neighbouring pixels can belong to different objects with different textures, so one group may need several textures at once. By default the hardware may assume the whole group reads one, and quietly sample the wrong texture for some pixels. Marking the index `nonuniformEXT` makes it loop over the distinct indices in the group instead. It is the classic bindless bug, so the qualifier is not optional.
- **Filling slots while frames are in flight.** Normally a descriptor set must not change while a submitted frame may use it. The bindless set is flagged update-after-bind, update-unused-while-pending and partially bound: a new texture may fill a slot no running frame reads, and slots never written are fine as long as nothing reads them. Textures are never unloaded, so a slot, once written, never changes.
These features are standard on desktop GPUs with Vulkan 1.3 drivers; the renderer checks for them at start-up and stops with an error on a GPU that lacks one.

**Lights.** Light records moved to storage buffers too, which lifted the 64-light limit the 4 KB uniform window set: the buffers now have room for 1024 point lights and 256 spotlights, each light carrying its own shadow (first tile, strength while fading, emitter size) instead of separate padded arrays. Room is not the same as cost, though: while each pixel looped over every seated light, the default stayed 64 point and 8 spot seats; clustered shading (below) made room and cost line up, and now every light the buffers hold is seated (see "More lights than seats").

**Culling casters by a light's reach.** A point light has six shadow views, and each used to test every object against its frustum. Most objects are nowhere near most lights, so a cheaper test comes first: an object can only shadow what this light lights if its bounding sphere touches the light's range sphere, `|c - p| < r + range` (compared squared, so no square root). That is one test per object and light, shared by all six faces, before the six frustum tests on the few survivors. In the lamp field it removed about one caster in five. It is nearly exact: a blocker lies between the light and a lit point, and lit points are within range, so a caster wholly beyond the range blocks nothing. The one exception taught something. A soft shadow's filter reads texels up to 12 around a point, and on a surface the light grazes, the surface's plane extended to those texels can pass beyond the range, where a dropped caster used to register. That only happens where the light has faded to almost nothing, and it changed 6 pixels of one golden image by three steps of a half float.

**Measured.** In the lamp field with every shadow tile redrawn (the worst case), recording a frame took the CPU 1.75 ms before the GPU scene and about 1.15 ms after, in the release build. The rest of that time is per-frame work other than draw calls, not yet broken down further. The upload shrank too: about 61 KB a frame, one record per object plus 4 bytes per draw-list entry, instead of a matrix per caster per tile. The GPU's own time hardly changed, since it draws the same triangles; the gain is what the CPU no longer has to say.

**Why the pictures changed a little.** Moving the matrix product from the CPU to the shader changes the order of the arithmetic: once `(view × model) × vertex`, now `view × (model × vertex)`. Mathematically equal, but each multiply rounds, so the two give vertex positions that differ in the last bit or so. Usually that is invisible. But a shadow test is a **threshold**: "is this point's depth beyond what the light saw?" At a point lying almost exactly on the threshold, a last-bit change flips one of a soft shadow's 16 samples from lit to blocked, and that pixel changes by a sixteenth. It is the same reason a ball balanced on a ridge falls left or right depending on a breath of wind: tiny causes, visible effects, but only at the knife edge. In the golden images about one pixel in a thousand changed, scattered through the lamp-lit pools, and in the final pictures a few dozen isolated pixels by at most 6 of 255 steps. The step was therefore taken in two commits: first the catalogue with the CPU's products kept (the images matched bit for bit, proving the new path right), then the products moved to the GPU and the references saved again.

### The depth prepass: settle who is in front, then pay for light

A pixel can be covered by several surfaces: the floor, a pillar in front of it, a lamp in front of that. Each surface a triangle lays over a pixel is a **fragment**, a candidate colour, and the **depth test** keeps the nearest. The waste is in the order. Drawing the floor first, the GPU shades its fragment in full (every light, every soft shadow), then the pillar arrives nearer and overwrites it. That thrown-away work is **overdraw**.

GPUs already fight it with **early depth testing**: when the fragment shader neither writes depth nor discards pixels, the hardware tests depth *before* shading and drops fragments behind what is already stored. That only helps when the near surface happened to be drawn first, and the renderer sorts draws by mesh (for instancing), not by distance.

The **depth prepass** makes the order irrelevant. First every opaque object is drawn writing depth only: no fragment shader, no colour. That is cheap (0.005 to 0.017 ms here), and afterwards the depth image holds the nearest surface at every pixel. Then the main pass draws everything again with the depth test set to **equal** and depth writes off: a fragment survives only if its depth is exactly the stored one, so `triangle.frag` runs once per pixel, for the surface you actually see. It is like surveying a site before building: one quick pass with a measuring tape settles where everything stands, and the expensive work happens only where it counts.

**The catch: equal means bit for bit.** "Equal" compares floats exactly, so both passes must compute each vertex's depth to the last bit. If they differ by one rounding step, the fragment fails the test and the pixel is left unshaded, a black speck. Two consequences:

- The prepass uses the main pass's own vertex shader, not the shadow pass's depth-only one. The shadow shader multiplies world matrices; the main pass subtracts the camera first (camera-relative rendering). Mathematically equal, but each multiply rounds, so the two give different bits, the same lesson as the GPU scene's re-saved pictures.
- Even the same source can compile differently in two pipelines: a compiler may fuse a multiply and an add into one **FMA** instruction (rounding once instead of twice) in one and not the other, for example because the main pipeline also needs the position for lighting. Declaring `invariant gl_Position;` in the shader is GLSL's promise that the same expression on the same inputs gives the same bits everywhere it is declared. Adding it changed 10 of the 12 golden images by a few edge pixels (the positions now round as separate multiplies and adds), and from then on every golden image is identical with the prepass on and off. On this NVIDIA driver the prepass happened to match even without `invariant`, but nothing guarantees that on other drivers.

**Lights from behind skip their shadows.** A light behind the surface adds nothing (Lambert's cosine is negative), and the BRDF already returned early for it, but only *after* the shader had paid for that light's soft-shadow lookup. `facesLight` now asks the same question first, on the same normal, so the picture is unchanged bit for bit while the wasted lookups go.

**What it bought.** The depth prepass is a setting (Passes in the debug UI; PREPASS in the F4 timings), so the two can be compared live. At 2560x1440 on the RTX 5080: the floodlights view, where the slab, obstacles and poles hide each other, went from 0.39 to 0.32 ms; the start and gallery views gained 3-4%; the lamp field stayed at 1.2 ms. There the cost is not overdraw: it is every pixel looping over all 64 point light seats, most of them far out of range. Clustered shading (next) fixes that, and it reads the prepass's depth to know how deep each part of the screen goes, as will ambient occlusion, reflections and temporal anti-aliasing.

### Compute shaders: the GPU as a parallel calculator

Everything above is a **graphics pipeline**: vertices go in, the rasteriser turns triangles into pixels, and the fragment shader colours each pixel. A **compute shader** drops all of that. There are no vertices, no triangles and no pixels, only "run this function N times, and here are the buffers to read and write". It is the GPU used as what it really is underneath, thousands of small calculators working in parallel, and it is what the engine's later physics on the GPU (cloth, probes for global illumination, sorting lights into clusters) will run on.

A few words describe how the work is laid out:

- An **invocation** is one run of the shader, like one particle in a simulation. Each knows its own number (`gl_GlobalInvocationID`) and uses it to pick its piece of the work.
- Invocations are launched in **workgroups** of a size fixed in the shader (`local_size_x = 64`). The GPU runs a group together on one of its cores (a streaming multiprocessor, in NVIDIA's terms). 64 suits NVIDIA, whose units run 32 invocations in lockstep, and AMD, whose units run 64.
- A **dispatch** asks for a number of whole groups. To run once per item, ask for the item count divided by 64, rounded up (`workgroupCount`). The last group then has spare invocations past the end, so every compute shader starts by checking its number and returning if it has no item.
- **Storage buffers** carry the data. Uniform buffers are small, read-only, and in this engine limited to 4 KB a block; storage buffers can be large and *writable*, which is how a computation hands back its results. Their layout follows the `std430` rules, where a `vec4` takes 16 bytes, exactly like four C++ floats in a row.
- **Push constants** are a few bytes written into the command list itself (how many items, which function), the cheapest way to pass a handful of numbers.

A `ComputePipeline` (`render/vulkan/vulkan_compute`) is the compiled shader plus the layout of what it reads and writes. With no fixed-function stages it is much smaller than a graphics pipeline.

Getting results back to the CPU needs the same care as images between passes. The output buffer lives in memory the CPU can read, but the GPU's writes may still sit in its caches. So the dispatch is followed by a barrier, "compute shader writes, then host reads", the CPU waits for the fence, and, on memory the CPU does not keep coherent, it invalidates its own view before reading. The other direction is simpler: whatever the CPU wrote before submitting is visible to the GPU, because submitting guarantees it.

The first users of compute shaders are the GPU tests (see Testing) and, inside every frame, clustered shading (next).

### Clustered shading: sorting the lights into boxes first

Before clustered shading, every pixel looped over every seated light, 64 of them, and asked each "do you reach me?". In the lamp field a lamp reaches 5 units and the lamps stand 3.5 apart, so any point on the ground is lit by about 4 to 8 of them, and the other 56 or so answers were "no", at every one of the 3.7 million pixels of a 2560x1440 frame. Each "no" is cheap, but there are a lot of them, and the count grows with lights times pixels.

The fix is the same idea as the physics broad phase (section 10) or the cell lists of a molecular dynamics code: sort first, then only compare neighbours. The view is cut into a grid of **clusters**, and before shading, a compute pass writes down for each cluster which lights can reach into it. A pixel then works out which cluster it is in and only asks those lights.

**The grid.** The screen is cut into square tiles of 64 pixels (40 x 23 at 2560x1440), and each tile is cut along the view direction into 24 **slices**. Slice k starts at depth near · (far / near)^(k / 24): each slice is the same *factor* deeper than the one before (1.33 for the demo's near 0.1 and far 100), not the same distance. That is a logarithmic scale, for the same reason as on a slide rule: near the camera a centimetre matters, far away a few metres do not, and perspective shrinks far things in just that proportion, so each cluster looks roughly as deep on screen as it is wide. Finding a pixel's slice is the formula run backwards: slice = floor(log(depth) · scale + bias), one logarithm per pixel.

**Is this light near this box?** Each cluster is a small piece of the view frustum. The compute pass puts a box around it and asks whether the light's sphere of reach touches the box: is the box's closest point to the sphere's centre within the radius? (The same test the physics uses for a sphere against a box.) A spotlight only lights a cone, so it is also tested against the cone, with a neat trick (Wronski, "Cull that cone"): split the way from the cone's tip to the cluster into its part along the beam and its distance from the beam, turn that picture by the cone's half-angle, and the result is the distance from the cone's slanted side. If that is more than the cluster's size, the beam misses. Both tests are **conservative**: they may say "maybe" for a light that in the end gives nothing, but never "no" for a light that reaches. A "maybe" only costs a pixel the old distance check, while a wrong "no" would leave a dark patch.

**One bit per light.** Each cluster's list is not a list but a row of bits, one per light: 1024 bits for the point lights and 256 for the spotlights, 160 bytes per cluster, about 3.5 MB for the whole grid. A bit is 1 when the light may reach the cluster. The pixel walks the bits from the lowest up, and the lowest bit is light 0, so it meets the lights **in the same order** as the old loop did.

That order matters more than it seems. Floating-point addition is not associative: (a + b) + c can differ from a + (b + c) in the last bit, so adding the same lights in a different order would change pixels slightly (that is how instancing changed a few golden images, see The GPU scene). Here the pixel adds exactly the lights that reach it (every one is in its list, and the lights it now skips added exactly nothing before), in exactly the same order, so the result is the same to the last bit. And it was: all twelve golden images matched with clustering on and off, which is a very strong test that the clusters never miss a light. There is one subtlety: a pixel near the boundary of two slices may compute its slice from a slightly rounded logarithm and land in the neighbour, whose box the light might just miss. So every box is grown by a thousandth of its depth on every side, more than any rounding, and the tests check, for thousands of random lights and points, that every light reaching a point is in that point's list.

**Inside the compute pass.** One invocation works out one cluster. Rather than each of them reading all 1024 lights from memory, the 32 invocations of a workgroup share the work: each fetches one light, turns it into the camera's frame, and leaves it in **shared memory**, a small, fast memory on the chip that the whole group can see (like a team spreading their papers on one table). A `barrier()` makes everyone wait until all 32 are on the table, then each invocation tests its cluster against all 32 and writes one 32-bit word of its row; another barrier, and the next 32 lights. Each invocation writes only its own words, so no two ever write the same memory, and the result never depends on which finishes first.

**Barriers for a buffer.** The bit rows live in a buffer only the GPU touches: the compute pass writes it, the main pass reads it. Like images between passes (Barriers, above), the GPU must be told the order, twice. Before the compute pass, last frame's main pass may still be reading the rows, so its reads must finish before they are overwritten (a *write after read*: only the order matters, since nothing written needs to reach anyone). After it, this frame's main pass reads what was just written (a *read after write*), so the writes must also be flushed out of the compute units' caches and become visible to the pixel shaders. A buffer has no layouts, so this is simpler than an image's barrier: stages and kinds of access only.

**Pixels in step.** The GPU shades pixels in groups of 32 (a **wave** on NVIDIA) that run in lockstep, like a rowing crew: if one pixel has to loop over 7 lights and its neighbour over 3, the neighbour waits. At the edge between two clusters, neighbouring pixels have different lists. So each word of bits is first combined across the wave with a bitwise OR (`subgroupOr`, one instruction), and all 32 pixels walk the combined list together, reading each light's record at the same moment. A pixel meets a few lights from its neighbours' lists this way, but those cannot reach it (they would be in its own list otherwise), so they add nothing and the picture stays the same. At today's light counts this measured neither faster nor slower; it is there for scenes where lights are dense and lists vary from pixel to pixel.

**What it bought.** At 2560x1440 on the RTX 5080, the lamp field's main pass went from 1.18 to 0.75 ms; with point light shadows off, from 0.62 to 0.30, half. Of what remains, most is the soft shadows of the lamps that really do reach each pixel (32 shadow samples each), which clustering cannot remove, since those lamps really do light the pixel. The compute pass itself takes 0.004 to 0.008 ms. Views with few lights got 10-13% faster. And since a light now costs only where it shines, every light the buffers hold is seated: the lamp field draws all 88 lamps in view in 0.78 ms, where the old loop would take 1.40.

**Seeing it.** The debug UI's Cluster heatmap (Passes) colours each pixel by how many lights its cluster lists: black for none, then blue, green, yellow and red at 16 or more. It showed something worth knowing: in the lamp field the near ground lists 8 to 12 lamps per cluster, and the far rows 16 or more, though only 4 to 8 reach any point. Far slices are deep and a tile seen at a grazing angle covers a lot of ground, so the boxes are loose there. Each extra lamp costs only a distance check today; if scenes ever get far denser, tighter clusters (using the depths the prepass already found) are the next step.

### Ray tracing: asking the GPU what a ray hits

Rasterising answers "which pixels does this triangle cover?", one triangle at a time. Global illumination needs the opposite question: "starting here and going that way, what is the first thing I meet?", asked millions of times a frame from points all over the level, not just from the camera. Testing every ray against every triangle would cost rays times triangles. Modern GPUs answer it in hardware with a **ray query**: a shader hands over a ray and gets back the distance to the first hit, which object and which triangle, where on the triangle (two **barycentric coordinates**, the weights that blend its three corners), and whether it struck the **front** or the **back**.

The GPU finds that quickly by searching a tree of nested boxes, an **acceleration structure** (a bounding volume hierarchy): if a ray misses a box, it misses everything inside it, so most of the scene is skipped after a handful of box tests, the same idea as the collision broad phase's grid but shaped to the geometry. It comes in two levels, like a library's catalogue. Each mesh gets a **bottom-level** structure around its own triangles, built once when the mesh is uploaded (and then **compacted**: copied into a buffer of exactly the size the finished tree needs, often half the first one). Each frame gets a **top-level** structure listing instances, each one a mesh's structure placed by a model matrix, which is cheap to rebuild from scratch every frame (about 0.1 ms for 900 objects), so moving objects need no special care. A ray first walks the top level to find which instances it passes near, then each instance's own tree, carried into that mesh's coordinates by the inverse of its matrix.

**Front and back.** A triangle's front is the side its corners go round anticlockwise when you look at it, which for a closed mesh is the outside. A ray that hits a back face therefore started inside something, which DDGI uses to find probes buried in walls and to stop light leaking through them. The GPU decides the side in the mesh's own coordinates, so a mirrored object still has its outside as its front.

**Masks.** Each instance carries a few bits, and each ray says which bits it wants to see. Shadow casters are "solid"; the rest, mostly the glowing faces of lights (the sun disc, the moon, lamp bulbs), are "visual only". Rays gathering light see only solid things, because those glowing objects stand for lights that are already added directly, and seeing them too would count their light twice. Reflections later will want to see them, and only need to ask for both bits.

**Finding the surface that was hit.** A hit says "instance 12, triangle 300, weights (0.2, 0.5)". To shade it, the shader needs that triangle's corners: their normals and texture coordinates. Every mesh's vertex and index buffers have a **device address**, their place in the GPU's memory, and a shader can follow an address like a pointer in C++. A small table lists each mesh's two addresses, the instance record says which mesh it is, and the shader reads the three corners and blends them with the hit's weights.

**Checking it.** A debug view (F5, Passes, Ray debug view) fires one ray per pixel from the camera and shows each hit's normal, its colour, or its side (green front, red back). It must look exactly like the normal picture, and it does. The GPU tests go further: 2,000 rays traced by the hardware are compared with the engine's own CPU raycast over the same triangles, and every one agrees on what was hit and from which side. Distances agree to a few millionths, except that rays grazing a surface at a shallow angle may differ more, since a tiny rounding in where the surface lies moves a grazing hit far along the ray (by that rounding over the cosine of the angle), so the allowance grows with one over that cosine.

Ray tracing is optional: on a GPU without it, none of this is made and everything that needs it stays off.

### Texture filtering and mipmaps

A texture is sampled once per screen pixel, and that is a sampling problem in the signal-processing sense:

- **Up close**, one texel covers many screen pixels. Taking the nearest texel shows hard-edged blocks, so the sampler uses **linear filtering**: it blends the four surrounding texels by distance.
- **Far away**, one screen pixel covers many texels. Taking just one of them is sampling below the Nyquist limit: fine detail folds back as shimmer and moiré, and changes every time the camera moves.

The cure for aliasing is to low-pass filter before sampling. A **mipmap** does that in advance: alongside the texture, the GPU stores copies at half size, quarter size and so on down to 1x1, each texel the average of a 2x2 block from the level above (only a third more memory in total). When drawing, the GPU estimates how many texels fall in one screen pixel and reads the level where that is about one. The name is from the Latin *multum in parvo*, "much in a small space".

Two refinements:

- **Trilinear filtering** blends the two nearest levels, so there is no visible line where one level hands over to the next.
- **Anisotropic filtering** (up to 16x) helps surfaces seen at a grazing angle, like the ground stretching away. There one screen pixel covers a long thin strip of texture, and a single mip level would blur it; several samples along the strip keep it sharp.

The levels are generated on the GPU when a texture is uploaded. `mipLevelCount(width, height)` gives the length of the chain: the original plus one level per halving of the long side.

### Gamma: storing light the way eyes see it

Our eyes respond to light roughly logarithmically, like ears to sound: doubling the light does not look twice as bright. So image files do not store amounts of light. They store **sRGB** values, roughly `light^(1/2.2)`, which spends more of the 256 levels on dark tones, where we notice small steps. A stored 128 is only about 22% of white's light, not 50%.

Lighting maths (adding lights, scaling by angle, averaging texels for mipmaps) is only correct on amounts of light, like adding intensities rather than decibels. So:

- Textures are created with an `_SRGB` format: the GPU converts each texel to linear light as it reads it, and mipmaps are averaged in linear light.
- Material colours are sRGB bytes too (what a colour picker gives), and `packMaterial` converts them with `srgbByteToLinear` (`core/srgb`).
- Light colours, intensities and emissive colours are already linear.
- The swapchain (the screen images) is sRGB as well, so the GPU converts the shader's linear result back as it writes each pixel.

A consequence when tuning: linear numbers for dim light look tiny. The demo's night ambient of 0.012 shows as about 0.11 of full brightness on screen, because the sRGB curve lifts dark values. Before this was done, the maths happened on sRGB values directly, which made light falloff and soft edges look too dark and too sudden.

### Frustum culling: skip what cannot be seen

A camera sees a **frustum**: a pyramid with its tip cut off, between the near and far planes and inside the field of view (for a shadow box, it is just a box). Anything entirely outside it would be drawn for nothing, so each frame the renderer skips it.

The frustum is six planes, each with an inside. They come straight out of the view-projection matrix: a point is on screen when its clip coordinates satisfy `-w <= x <= w` and so on, and each of those six inequalities is one plane. Every object gets a **bounding sphere** (its mesh's bounding box, placed by its model matrix, with the radius grown by the largest scale). A sphere is outside if its centre lies more than one radius beyond any plane: six dot products per object, far cheaper than drawing it.

One rule matters: each view culls with its **own** frustum. The main pass uses the camera's, but each shadow tile uses its light's. Something behind you can still throw a shadow into view (the low sun behind a tree behind you), so culling shadow casters with the camera's frustum would make shadows vanish whenever their caster leaves the screen, a classic bug. The test is also deliberately generous: near a frustum's corners it can keep a sphere that is just outside, which costs a little wasted drawing, never a missing object.

### Debug drawing

Much of what the engine knows is invisible: bounding boxes, light positions, where a character is walking, and later colliders and paths. **Debug drawing** shows it as wireframe lines over the scene. Anything can add shapes to a `DebugDraw` (lines, boxes, spheres drawn as three circles, capsules), and it is emptied every frame, so a shape stays only as long as something keeps adding it. That style is called **immediate mode**: instead of creating a "debug box object" and remembering to delete it, you just say "draw a box here" every frame you want it.

The lines are drawn last, unlit, and ignore depth, so they show through walls. That is the point: a collider inside a wall or a light behind a hill is exactly what you want to see when debugging. Games add their own shapes in `onDebugDraw`; the engine itself can draw a box around every mesh (it knows each mesh's bounds) and how many meshes culling skipped last frame, which the demo switches on with F4.

That readout is text, and the engine has no font yet. Debug text is drawn with lines too, in a **stroke font**: each character is a few straight strokes on a 4 by 6 grid, like a plotter or an old vector arcade game. These lines are placed in window pixels instead of the world, and drawn with a flat projection where (0, 0) is the top-left corner.

### The debug UI: one description, many uses

Every number someone might tune (bloom strength, exposure, how many lights get shadows, mouse sensitivity) lives in a settings struct next to the system that reads it. Changing one used to mean editing the code and recompiling. Pressing F5 now opens a window with a slider, checkbox or list for each one, and the change shows on the next frame.

The interesting part is that no setting has any UI code of its own. Each settings struct has one `visit()` function that lists its fields once, the way a physicist writes down a quantity: a name, the value, the range that makes physical sense and the unit. For example, "bloom strength, between 0 and 1, useful up to 0.2". A **visitor** is anything that walks that list and does one job with each entry. The engine has several:

- the **UI visitor** draws a slider for each number, a checkbox for each switch and a list for each choice;
- the **saving visitor** writes each value as text, and the **loading visitor** reads text back into the fields;
- the **checking visitor**, used by the tests, confirms every default lies inside its stated range.

So the description is written once and every use is derived from it. Adding a field to `visit()` gives it a slider, a place in the settings file and a test, with nothing else to remember. This is the **visitor pattern**: the walk over the data is written once, and new operations are added as new visitors.

Each number carries two ranges. The **hard limits** say what is meaningful at all (a share of light cannot be above 1), and loading a hand-edited file clamps to them. The **slider range** is the useful part, which can be much narrower, so the slider is not fiddly. A few settings are read only once, at start-up (the simulation rate, the graphics card's validation mode). The UI shows those greyed out rather than pretending a change takes effect.

**Save** writes only the values that differ from the defaults to `settings.cfg`, and the engine loads that file at every start before anything reads the settings. Saving only the differences matters: if a default is improved later, everyone who never touched that setting gets the improvement, and deleting a line from the file returns that setting to its default. The **quality preset** (Low, Medium, High, Ultra) is a shortcut that sets the costly settings together. Ultra is the engine's own tuning; High is the same with the sun's shadow cascades at half the resolution (a third of the shadow memory), for typical graphics cards, and the lower ones give up more shadow detail and the widest bloom first. Settings that are a matter of taste, like exposure, are never changed by a preset.

The UI itself comes from Dear ImGui, a widely used library for tools. It is **immediate mode**, like debug drawing: every frame the code simply says "a slider for this value here", and ImGui reports whether the slider moved. There are no UI objects to create, keep in sync or delete, which is why one visitor can draw the whole window.

The UI is drawn into an image of its own, then laid over the finished frame last. Two reasons:

- **HDR.** On an HDR screen the frame is in scRGB, where 1 means a dim 80 nits. ImGui's colours drawn there directly would come out dim and wrong. As a separate layer, its colours are decoded from sRGB and scaled to the screen's white level, so the window looks as it would on the desktop.
- **Premultiplied alpha.** Drawn onto a transparent image, each pixel ends up holding colour times coverage, plus the coverage itself. The final blend is then a single formula, `ui + screen * (1 - coverage)`, like a sheet of tinted glass that lets through the share of light it does not block.

The HUD (phase 9) will go the same way. As with tone mapping and bloom, the blend's maths is mirrored on the CPU and tested.

### Dithering: noise against banding

Even with gamma, the screen has only 256 levels per channel. A slow, dark fade, such as the flashlight's pool on the ground at night, changes by less than one level over many pixels, so whole patches round to the same level and then jump together: visible rings, or **banding**. It is quantisation, like measuring a gentle slope with a ruler marked only in whole centimetres.

**Dithering** adds a little noise before the rounding, up to half a level either way and different for every pixel. Near a boundary, some pixels then round up and some down, in proportion to where the true value lies between the two levels, and the eye averages them back to the in-between brightness (the same idea as halftone printing, or dither in audio mastering). The noise is sized in the screen's own steps, so the shader encodes to sRGB, adds it, and decodes again. It comes from a formula on the pixel's position (**interleaved gradient noise**), which spreads values evenly over every small patch, so the grain is fine and even. A value exactly on a level never moves, so flat colours stay clean, and on average nothing gets brighter or darker; `core/dither` tests both.

### Depth range and reversed-Z

`Mat4::perspective` follows the OpenGL convention: depth runs from -1 at the near plane to +1 at the far plane. Vulkan wants 0 to 1. `toGpuDepthRange` converts inside the renderer, so nothing outside `render/` knows about it, and it turns the range round while it is at it: the near plane is stored as 1 and the far plane as 0. This is **reversed-Z**.

Why turn it round? A perspective projection does not store distance itself but something like 1/distance (that is what makes far things small on screen, and the depth comes from the same divide). So depth changes quickly close to the camera and hardly at all far away: two surfaces 1 cm apart at 1 km differ in depth by about one part in a billion. A float, meanwhile, is a mantissa times a power of two, so it has as many values between 0.25 and 0.5 as between 0.5 and 1, and its values crowd towards 0 (like marks on a logarithmic ruler). With the far plane at 1, distant depths land just below 1, where floats are coarsest (about 6e-8 apart): the two surfaces get the same depth and flicker through each other, which is called **z-fighting**. With the far plane at 0 they land near 1e-4, where floats are a thousand times finer. The 1/distance squeeze and the float's crowding towards 0 nearly cancel, like reading an inverse-square signal on a log scale, and precision comes out almost even from near to far: about 270 distinct floats between those two surfaces instead of none.

Turning the range round turns every comparison round too: nearer now means a *larger* depth. The depth test keeps the greater value, the depth image starts cleared to 0 (nothing in the way), shadow tiles are reset to 0, and the shadow lookup counts a point as lit when its depth is at least the stored one. The far plane stays where the camera puts it; reversed-Z would also allow an infinitely far one, which is a later option.

### Camera-relative rendering

A float keeps about seven significant digits. Near the origin that is far more than enough, but 12 km away the gap between neighbouring floats is about a millimetre. The obvious way to draw puts the camera's position into the view matrix and each object's position into its model matrix, and multiplying the two matrices then subtracts one from the other: two numbers near 12,345, each already rounded to a millimetre, whose difference is a few metres. What is left of the rounding shows as vertices jittering by a fraction of a pixel as the camera moves (measured: about a quarter of a pixel at that distance).

So the renderer subtracts first. Every position the main shaders see is measured from the camera: the model matrix gets the camera's position taken off its translation, the view matrix keeps only the camera's turn, and lights and shadow lookups are shifted the same way. The world's axes stay the same, so normals and light directions do not change at all. The subtraction of two nearby floats is exact to within the tiny rounding of its small result, so the screen position comes out about 20,000 times closer to the exact answer. The shadow maps themselves are still drawn in world space, because shadow caching recognises a tile by its matrices, and camera-relative ones would change every time the camera moved. This is groundwork for long FPS sightlines and the solar system, where the subtraction moves to doubles (the world's positions are still floats today).

### The normal matrix

Positions are transformed by the model matrix `M`. Normals cannot be: stretch an object along x and a 45 degree slope gets shallower, so its normal must tip towards vertical, but `M` would tip it the other way.

The physics view: a normal is not an arrow like a position, it describes an **area element** (like a cross product of two tangent vectors), and areas transform differently from lengths. The correct matrix is the **inverse transpose** of `M`'s 3x3 part. For pure rotations it equals `M`, which is why the difference only shows under non-uniform scale.

`normalMatrix(model)` computes it once per object on the CPU (not once per vertex in the shader) as the **cofactor matrix times the sign of the determinant**. Normals are normalised afterwards, so the determinant's size does not matter, but its sign does: a mirrored object has a negative determinant, and without the sign its normals would point inwards and it would be lit from behind.

---

## 9. Lighting and shading

### Lights are components

- `DirectionalLight`: a light so far away its rays are parallel, like the sun. It stores the direction its light travels and needs no `Transform`.
- `PointLight`: shines in every direction from its entity's `Transform` (a torch, a muzzle flash). It has a `range` where it fades to exactly zero and a `sourceRadius`.

Every light except ambient has a `castsShadows` flag, on by default.
- `SpotLight`: a point light that shines in a cone, like a flashlight. It stores the direction of its beam and two cone angles.
- `AmbientLight`: a flat fill that stands in for light bounced around the scene. Several add up.

Light colours are 0..1 tints with a separate, unbounded `intensity`, keeping "what tint" apart from "how bright". Material colours are different: they are reflectances, the fraction of light a surface bounces back, so 0..255 fits them.

Each frame, `collectLighting` gathers lights from the `World` (point light positions interpolated like meshes, so a light carried by something moving does not jitter), and `packLighting` turns them into the shader's layout. The light buffers hold up to **4 directional, 1024 point and 256 spotlights**, and by default 64 point and 8 spotlights get a seat; when a scene has more, the renderer picks the ones that matter most for the frame (see "More lights than seats", below).

### Diffuse: Lambert's cosine law

A matte surface scatters light evenly in all directions, so it looks equally bright from any viewpoint. How much light it *receives* depends on its angle to the light: light hitting at a slant spreads over more area, so each bit of surface gets less. That is Lambert's cosine law:

```
diffuse = lightRadiance * max(0, N · L)
```

where `N` is the surface normal and `L` the direction towards the light (both unit vectors).

### Point lights: inverse square, with a window

Real light from a point falls off as 1/d², because the same energy spreads over a sphere of area 4πd². Two practical changes:

```
falloff = window² / (d² + r²),   window = clamp(1 - (d / range)⁴, 0, 1)
```

- `r` is the light's `sourceRadius` (1 by default). It keeps the light finite right at the surface: a real bulb is not a mathematical point, and close to a large source the light arrives from its whole area. Far away (d much larger than r) it is ordinary inverse square.
- Pure 1/d² never reaches zero, so every light would have to be computed for every pixel. The window brings it smoothly to exactly zero at `range`, with zero slope, so there is no visible edge.

### Spotlights: a cone with a soft edge

A spotlight is a point light whose brightness also depends on the angle between its beam's axis and the direction to the surface:

```
cone = smoothstep(cos(outerAngle), cos(innerAngle), dot(-L, beamDirection))
```

Inside the inner angle it is at full brightness (the bright core), beyond the outer angle it gives nothing, and in between it fades smoothly (the penumbra). A real flashlight has that soft edge because its bulb is not a perfect point: different parts of the bulb light slightly different cones, and their overlap blurs the boundary. A hard cutoff would look like a stencil.

The comparison uses cosines rather than angles (a bigger cosine means closer to the axis), so the shader needs no `acos` per pixel; the two cosines are computed once on the CPU. The cone multiplies the same distance falloff as a point light, including the source radius. The demo's flashlight uses r = 4.5, so it is gentle up close but still reaches across the scene.

The demo's flashlight is held in the right hand, a little to the side of and below the eye, and aimed at the middle of the view. If it sat exactly at the eye, every shadow it casts would hide straight behind the thing casting it, out of sight. A light that follows the camera is placed every frame in `onUpdate`, not in ticks: the camera moves per frame, and updating the light per tick would make the beam trail behind mouse look. When the duck carries it in MOBA mode, it uses the duck's interpolated pose, for the same reason moving lights are interpolated.

### Physically based materials: what a surface does with light

Every pixel answers one question: of the light arriving from direction `L`, how much leaves towards the eye along `V`? The function that answers it is the **BRDF** (bidirectional reflectance distribution function), `f(L, V)`, and a light's contribution is `f · E · (N · L)`, with `E` the light's irradiance and `N · L` Lambert's cosine from above. The engine uses the model glTF files are authored for, **metallic-roughness**, so a material is just three numbers (plus textures that vary them across the surface): a base colour, how **metallic** it is (0 or 1, in-between only for blends such as dusty metal), and how **rough** (0 a polished mirror, 1 fully matte).

**Microfacets.** Up close, every surface is a landscape of tiny perfect mirrors facing in random directions. A facet can send light from `L` into the eye only if its normal lies along the **halfway vector** `H = normalize(L + V)`, by the law of reflection. That gives the specular part as a product of three factors, each answering a physical question:

```
f_specular = D(H) · G(L, V) · F(V · H) / (4 (N · L)(N · V))
```

- **D, the distribution (GGX):** what fraction of facets point along `H`? A probability density over directions, `D = α² / (π ((N·H)²(α² − 1) + 1)²)` with `α = roughness²`, normalised so the facets' area projected onto the surface equals the surface's own area. Smooth surfaces have a narrow peak (a small, bright highlight), rough ones a wide one. GGX has long tails, so highlights get the soft glow around them that measured materials show.
- **G, shadowing and masking (Smith, height-correlated):** of those facets, how many can both the light and the eye actually see? At grazing angles the bumps hide each other, like a field of tall grass seen edge-on. The height-correlated form counts a facet hidden from both once, not twice: a deep valley tends to hide from both together.
- **F, Fresnel (Schlick's fit):** how much does a mirror reflect rather than let in? It follows from Maxwell's equations at the boundary; Schlick's `F = F₀ + (1 − F₀)(1 − V·H)⁵` fits them closely. `F₀`, the reflectance head-on, is about 4% for glass, plastic, stone and paint (refractive index 1.5), and every surface becomes a mirror at grazing incidence: a lake at sunset, a wet road ahead of you.
- The `4 (N·L)(N·V)` is geometry: the change of variables from facet normals to reflected directions, and the projection onto the surface.

**Diffuse and metals.** Light that gets *into* a non-metal scatters around under the surface and comes back out in all directions: the diffuse part, Lambert's `colour / π`. It only gets what the mirror part did not reflect, so it is weighted by one minus the specular albedo, and energy is conserved. Metals are different: their free electrons absorb refracted light within nanometres, so **a metal has no diffuse part at all**, and its colour is in its reflection instead (gold's `F₀` is about 1.00, 0.77, 0.33). glTF's metallic blends the two: `F₀ = mix(0.04, baseColour, metallic)`, diffuse colour `baseColour · (1 − metallic)`. That is why highlights on red plastic are white (4% of white light), but on gold they are gold.

**Light lost between facets.** The model above counts only light that bounces off one facet and leaves. On a rough surface, much of the light reflected by one facet hits another and bounces again before escaping; the single-bounce model drops it, so rough metals come out too dark (a perfectly white, fully rough metal returned only 33 to 64% of the light, depending on the view). The fix (Turquin 2019) scales the specular part by `1 + F₀ (1 / E − 1)`, where `E` is how much a perfect mirror's facets return from that view: exactly the missing share for a white metal, and close for coloured ones. `E` depends on the view angle and the roughness only, and has no formula, so the engine **integrates it numerically at start-up** into a small 32 × 32 table (`buildBrdfTable`): for each entry it averages 4096 reflected directions, drawn more often where `D` is large (importance sampling, the Monte Carlo trick of sampling in proportion to the integrand so `D` cancels) along a Hammersley sequence (evenly spread points, which converge much faster than random ones). It takes about 85 ms in the release build. The shader reads the table once per pixel.

**The white furnace test.** The physics check in the tests: put a surface that absorbs nothing inside a furnace that glows the same in every direction, and it must send back exactly the light it receives, from every view, at every roughness; any more would create energy, any less would mean the model loses it. The tests integrate the shader's BRDF numerically over the whole sky and check that a white metal and a white non-metal both return 1 (to within 2%). Other tests check that GGX integrates to 1, that swapping the light and the eye changes nothing (Helmholtz reciprocity), and Fresnel's two limits.

**Units.** A textbook BRDF has the `1 / π` in Lambert's term, which would make every scene 3.14 times darker than the earlier shading. The engine instead multiplies the whole BRDF by π, which is the same as saying a light's value is the irradiance it gives a surface facing it: a white matte surface lit head-on by a light of 1 shows 1, so lights and exposure tuned before kept their look. Unreal and Frostbite fold π into their light units the same way.

**Ambient.** Ambient light comes from every direction alike, which is the furnace situation, so a surface returns exactly its albedo of it: the weighted diffuse colour plus the specular albedo from the table. Without the second term a metal would be pitch black wherever only ambient light reaches it. Real surroundings are not uniform, so this is a stand-in until reflections of the scene exist; global illumination (phase 6) will replace the diffuse half.

**Textures.** glTF stores roughness and metallic per texel in one texture's green and blue channels. Those are plain numbers, not colours, so unlike colour images they must not be sRGB-decoded: they are the engine's first **linear** textures (`TextureEncoding::Linear`, uploaded as plain `UNORM`). The same file asked for both ways becomes two textures.

The demo's teapot is real gold (metallic, roughness 0.35); grass, stone and the course have roughnesses to match the old shininess settings; glowing things (the moon, bulbs, floodlight heads, the marker) are black and fully rough, so lights reflect off them too faintly to see.

### Emissive: surfaces that glow

Everything above is light a surface *reflects*. A glowing surface, like the moon, a lamp or a screen, also *emits* its own, which does not depend on any light reaching it. The material's `emissive` colour is simply added at the end. Give a glowing object a black base colour and it shows only its emitted light, so it looks the same day or night (the demo's moon and its MOBA marker).

Directly, emissive surfaces do not light their surroundings: the moon's glow is just its own colour, and the actual moonlight comes from a separate `DirectionalLight`. Where there are light probes (global illumination, below), a glowing surface that casts shadows does light what is around it, through the probes; the glowing stand-ins for real lights (the sun disc, the moon, bulbs) are left out of that, or their light would be counted twice. The soft halo you expect around a bright light comes from a post-processing effect, bloom.

### The whole sum

```
colour = ambient · (diffuse colour + specular albedo) + Σ (diffuse + specular) per light + emissive
```

Light adds up linearly, like superposing intensities, so every light simply adds its share. A surface with no usable normal gets ambient and emissive light only. Inside a light probe volume the first term changes: the probes' measured light replaces the flat ambient, in the diffuse part and the specular part separately (see global illumination, below). The whole sum happens in linear light (see gamma, above), into a floating-point image that tone mapping turns into the screen image afterwards.

### Shadows: a depth photo from the light

A point is in shadow when something sits between it and the light. Testing that directly for every pixel against every triangle is ray tracing. **Shadow mapping** gets the answer from one extra drawing of the scene instead:

1. Put a camera at the light and draw the scene keeping only depth. The result, the **shadow map**, records for every direction how far the nearest surface is from the light. It is like a photo taken with a rangefinder.
2. While drawing the camera's view, project each pixel's point into that photo and compare: if the photo saw something nearer to the light in that direction, something is in the way, so this light does not reach the point.

Each kind of light needs a different camera:

- A **spotlight** gets one perspective camera looking down its beam, just wide enough for its cone.
- A **point light** shines every way, so one photo is not enough: it takes six, one per face of a cube around the light, each a square 90 degree perspective camera looking along +X, -X, +Y, -Y, +Z or -Z. A direction belongs to the face of its largest component (`pointShadowFace`).
- A **directional light** (the sun) has parallel rays, so its camera has no perspective: it is a box (an **orthographic** projection), like photographing with light that never spreads. The sun lights the whole world, but a shadow map has finite resolution, so it gets several boxes, small and sharp near the camera and larger further out (**cascades**, the next section). Only the brightest directional light casts shadows; any others still light the scene.

All these views share one depth texture, an **atlas** of 8192 x 4096 texels (128 MB), because SDL_GPU, which the engine started on, could not draw into one layer of a texture array (Vulkan can; the atlas stayed as it was through the port). With the sun's cascades at their default 4096 pixels (the Ultra quality preset) the atlas grows to 8192 x 12288 (384 MB) to make room for them. Each frame `planShadows` cuts it into square tiles: four for the sun's cascades, one per spotlight and six per point light, for up to 8 spot and 64 point lights: every point light seat in the shader can have a shadow.

Tiles come in sizes from 256 to 1024 pixels (2048 for spotlights), chosen by how far the light reaches (`shadowTileSizeFor`): enough texels that one covers about 0.04 units across the far end of the view (0.02 for spotlights). A lamp with a range of 5 has cube faces 10 units across at their far end, so 256 texels do; a floodlight with a 35 degree cone reaching 25 units is 35 units across there, so it gets 2048. Spotlights aim finer because their beams usually land on the ground at a slant well short of their reach, which stretches each texel along the ground (at 1024 the floodlights' shadows of steps and poles came out visibly jagged), and because there are few of them and they usually stand still, so their big cached tiles cost nothing to keep. A 256 tile holds a sixteenth of the texels of a 1024 one, so small lights are cheap, which is what makes 64 shadowed point lights fit. If the lights still ask for more than the atlas holds, the least important ones' tiles are halved until everything fits: point lights first, then spotlights, and only when nothing else can shrink do the sun's cascades, never below 1024. The tiles are then packed like a **quadtree**: the atlas starts as squares of the largest size (2048, or 4096 in the grown atlas), a tile takes the smallest free square that fits, and a square that is too big is split into four quarters, as often as needed. Because every size is a power of two and the biggest tiles are placed first, the quarters of a split square always get used before another is split, so the atlas never ends up with unusable gaps. The shader is told each tile's rectangle in the atlas. The shadow pass draws the scene once per tile, all in one render pass, moving the viewport to each tile. Perspective views are made slightly wider than needed, so the samples around a point near a tile's edge stay on that tile. Models with `castsShadows = false` are left out of the shadow pass (the moon sphere, which surrounds its own light, would otherwise block everything), and the duck holds its flashlight out in front of it for the same reason.

The cost grows with every shadowed light: each tile is one more drawing of every shadow-casting mesh, so a shadowed point light costs up to six. That is why real games shadow only the lights that matter most, and why the cap exists.

Not every cube face is needed, though. A face can only shadow a pixel you can see if its view pyramid overlaps the camera's, and a lamp near you typically has one face pointing back past you at nothing on screen. `planShadows` tests each face with `Frustum::mayIntersect` and gives a face that fails no tile and no draw. The test is the same "all on the wrong side of one wall" idea as culling a sphere: a view is a convex shape, the space between its eight corners, so if all eight corners of one view lie outside one side of the other, the two cannot meet. It is checked both ways round, since a small view next to a corner of a big one is only caught by its own sides. Like the sphere test it can occasionally keep a face that just misses, but never drops one that is needed. A skipped face keeps its place in the list of tiles (with an empty rectangle), so the shader still finds face `f` of a light at its first tile plus `f`, and it counts anything there as lit.

Most lights and most of the world stand still, so most shadow tiles would come out exactly the same as last frame. **Shadow caching** keeps them: the atlas is not wiped between frames, and a tile is drawn again only when something it shows changed. Two pieces make that work:

- **Tiles keep their place.** `ShadowAtlasLayout` remembers which square each view had (named by its light's entity and its face) and hands the same square back while the view asks for the same size. Squares no longer wanted are given back, and four free quarters join up again into their parent square (a **buddy allocator**, after the paired halves in memory managers), so the atlas does not crumble into scattered small holes. If it ever does crumble so a new big tile has no room even though there is enough area in total, everything is packed afresh once, which always fits, and every tile is drawn again that frame.
- **The renderer remembers what it drew where.** For each square, `ShadowTileMemory` keeps the view matrix it was drawn with and a **hash** (a 64-bit fingerprint) of the casters inside it: their meshes and model matrices. Each frame each tile's casters are found and fingerprinted again, which is cheap next to drawing them; if the matrix and fingerprint match, the image already in the atlas is right and the tile is skipped. A light that moves changes its matrix; a box that moves, appears or vanishes in view changes the fingerprint. Drawing a tile forgets anything remembered in squares it overlaps, since it painted over them.

Since the atlas is kept, a tile that is redrawn must first be wiped by itself, while the clear at the start of a pass wipes the whole image. So each redrawn tile first gets one big triangle at the far depth (0, with reversed-Z) drawn with the depth test switched off, which overwrites exactly that tile. When nothing needs drawing, the shadow pass is skipped completely. The F4 view shows how many tiles were drawn this frame, how many were kept from earlier frames, and how many cube faces were skipped.
Three practical problems, and their fixes:

- **Shadow acne.** Each texel of the shadow map stores one depth for a whole patch of surface. Tested against itself, a sloped surface comes out half in front of and half behind its own stored depth, in stripes. The fix is a small **bias**: the lookup nudges each point off its surface along its normal by 2.5 texels of its tile (more for points far from the light, where texels cover more ground), so it tests a point just above the surface. GPUs can also bias the shadow pass itself, pushing depths away from the light more on steep slopes, and the engine used to. But that slope-scaled push has no upper limit: a pillar's side seen almost edge-on from a lamp above it got pushed back behind the ground, and its shadow came loose from its base (**peter-panning**, after the boy whose shadow came off). Measured in texels, the normal offset stays in proportion with every tile size. The sun is the exception: its lookups move towards the sun instead of along the normal (next section), because a push along the normal also moves the point sideways, and cascades with different texel sizes then disagreed about where a shadow's edge was.
- **Jagged edges.** A shadow map has a finite resolution, so a plain in/out test draws staircase edges. The sampler does the comparison itself and blends the results of the four nearest texels, and the shader averages nine such lookups in a 3x3 grid (**percentage-closer filtering**, PCF): at an edge, the fraction of samples that are lit becomes a smooth gradient.
- **Soft shadows that look real.** A fixed blur gives every shadow the same edge, but real ones are sharp where an object touches the ground and blur further away. The reason is that lights glow over an area, not from a point. Behind an object's edge there is a band that sees part of the glowing area but not all of it, the **penumbra**, like the ring of partial eclipse around the moon's total shadow on Earth. By similar triangles its width is `emitter radius × (receiver distance − blocker distance) / blocker distance`: zero where the blocker touches the receiver, widening as the gap grows, and wider for bigger glowing areas. Point lights and spotlights get this (**percentage-closer soft shadows**, PCSS) in three steps per pixel: search the shadow map around the point for things nearer the light and average how far they are (this reads the atlas a second time as plain depths, since a comparison only says yes or no); work out the penumbra width from the formula; then average 16 depth comparisons spread over that width. The 16 points are a **Poisson disc** (spread out with no two close together, like seats taken in an empty cinema), turned by a different angle in every pixel so the leftover error looks like fine grain instead of bands. A wide filter brings back acne in a new form: on a surface sloping away from the light, a sample a few texels over is legitimately further from the light, and compared against the centre's depth it looks shadowed. So each sample is compared with the surface's own plane at that spot instead (**receiver plane depth bias**), found from how the surface position changes between neighbouring pixels. Each light has an `emitterRadius` for this, its real glowing size (0.05 for a bulb, 0.025 for a flashlight by default), kept separate from `sourceRadius`, which only shapes the falloff near the light and is tuned much larger. The sun gets the same treatment with its own geometry (next section).
- **Resolution.** Tiles are 256 to 2048 pixels across (4096 for the sun's cascades), sized by the light's reach. How much ground one texel covers depends on the view: a lamp with a range of 5 on a 256 tile covers about 0.04 units per texel at its reach, and less nearer the lamp. A point light far from the scene is much worse: when the demo's moon was briefly a point light 64 units away, its 90 degree cube faces spread one texel over about 0.13 units, and most faces only saw empty sky. Faces outside the camera's view are now given back (above), but fitting point light views to where the scene actually is remains a possible improvement.

### Cascaded shadows: the sun near and far

One shadow map cannot serve the sun well. Its texels are the same size everywhere in the box, while what the camera sees shrinks with distance: a metre of ground fills hundreds of pixels at your feet and a handful at 200 m. A box big enough for the distance is far too coarse up close, and one sharp enough up close covers only a few metres. The answer is the same trick a map book uses (a city map, a country map, a world map): cut the view into slices along its depth, **cascades**, and give each slice its own box, small and sharp near the camera, large and coarse far away. The sun gets four, each the largest tile size, out to `ShadowSettings::directionalDistance` (200 m by default).

**Where to cut.** Evenly spaced cuts waste texels: the far slices are as long as the near ones but cover little of the screen. Logarithmic cuts, each slice reaching the same multiple further than the last (0.1, 0.7, 4.5, 30, 200 m), keep a texel's size on screen about the same in every slice, but squeeze the first slice into the last metre. The usual compromise, the **practical split scheme** (Zhang et al., 2006), blends the two: `split = λ × logarithmic + (1 − λ) × even`. The engine uses λ = 0.95, which cuts at about 3, 9 and 36 m (`cascadeSplits`).

**How big each box is.** The obvious box is the tightest one around the slice as the sun sees it. But that box changes shape whenever the camera turns, so the texels change size every frame and shadow edges swim. Instead each slice is wrapped in its smallest **bounding sphere**, and the box is the sphere's shadow, a square as wide as the sphere. A sphere looks the same from every direction, so turning the camera never changes the box's size. Its centre lies on the view's axis where the slice's near and far corners are equally far away: with corners `d × t` off the axis at distance `d` (t combines the tangents of half the view's width and height), equal distances give `z = (f + n)(1 + t²) / 2`; if that lands beyond the far end, the far end's centre is the best place (`cascadeSphere`). Then, as for the old single box, the box's centre is snapped to whole texels across the sun's direction, so as the camera moves it jumps a texel at a time and edges stay put. The price of stability is size: for a typical 70° view the sphere's radius is about 1.43 times the slice's far distance, so a slice ending at 36 m gets a box about 103 m across. That sets the texels: about 5 cm at 36 m on a 2048 tile (0.44 cm in the first cascade), half that on 4096.

**Which cascade a pixel uses.** The boxes overlap, and near the edges of a slice the finer box often still holds the point, so the shader takes the **finest box that holds it**, not simply the slice its distance falls in. Near that box's edge it hands over gradually: over a **blend band** (10% of the radius) it looks the shadow up in both cascades and mixes them, so there is no visible line; past the last box the shadow fades out to lit. A dithered switch is a setting too: each pixel takes one cascade, the next more often nearer the edge, which costs one lookup instead of two but looks grainy until temporal anti-aliasing (phase 6e) smooths it. All of this is in `cascades.glsl`, mirrored and tested on both sides like the other shader libraries.

**Bias along the light.** The normal offset against acne (above) moves a lookup point along the surface's normal by a few texels. On the ground under a low sun most of that push is sideways as the sun sees it, and a sideways push moves where the shadow's edge is found. With texels of 1 cm in one cascade and 6 cm in the next, the two cascades put a building's shadow edge up to 18 cm apart, and the seam showed as a step. Pushing the point straight towards the sun instead changes only how deep it is in the shadow map, never where it lands, so every cascade finds every edge in the same place. Surfaces the sun grazes need more depth bias (a texel then spans more depth on them), so the push grows with the tangent of the angle between the normal and the sun, capped at 10 (`sunShadowBias`).

**Soft edges from the sun's size.** The sun is not a point: from the ground its disc spans about half a degree (`DirectionalLight::angularRadius`, 0.267° either side of its centre). Behind a blocker the penumbra therefore widens by `tan(angular radius)` per metre of gap, about 4.7 mm per metre, the same at any distance from the sun. It is the reason the dappled spots under a tree are little images of the sun, blurred by the same angle. The shader runs the same three PCSS steps as for lamps, measuring distances inside the box, and works out the penumbra in metres before turning it into texels of whichever cascade is in use, so a shadow keeps its softness across a cascade boundary wherever the coarser cascade can still show it.

**The cost, and the quality presets.** Each cascade is one more drawing of the shadow casters in its box. Most frames cost little: a box that has not moved (the camera moved less than a texel) keeps its image from the last frame, like any cached tile. The Ultra preset, the default, gives each cascade a 4096 tile (texels from 0.22 cm near the camera to 14 cm at 200 m), and its atlas takes 384 MB of video memory. High uses 2048 tiles in the usual 128 MB atlas, Medium three cascades reaching 120 m, Low two of 1024 reaching 60 m. Changing the resolution rebuilds the atlas on the spot (one frame redraws every shadow).

**Fitting the cascades to the screen.** A slice's box is set by where the slice ends, whatever is actually on screen, so inside a room the cascades would still share out their texels over 200 m of view, most of it behind the walls. **Sample distribution shadow maps** (Lauritzen 2011) measure how far away the visible surfaces really are and end the cascades there. After the main pass a compute shader reads the whole depth image and keeps the farthest surface (sky pixels, left at the far depth, do not count). Thousands of workgroups each find their own farthest and nearest depth and fold them into one shared answer with atomic minimum and maximum, which give the same result whatever order the groups finish in. Atomics work on integers, and a positive float's bits read as an integer sort in the same order as the number, because the exponent sits above the mantissa: like comparing two numbers in scientific notation, the power of ten decides first. In the light lab's room the farthest surface is about 10 m away, so the cascades end at 11.6 m and every texel shrinks 7 to 17 times (the finest from 0.22 cm to 0.03 cm). Looking out to the horizon nothing changes, since the ground really does reach past 200 m.

Three choices make it behave. First, the answer arrives a frame or two late: the CPU plans each frame's shadows before the GPU has drawn that frame's depth, so it uses the last finished frame's measurement (building the cascades on the GPU instead would take the CPU's culling and the shadow cache away). Second, the distance is not used as measured: it is grown by 10% for that lateness and rounded up to a ladder of fixed steps, each 1.25 times the last, and it climbs a step at once but comes down only when the need has dropped two steps. That is hysteresis, like a thermostat that switches on at 19° but off only at 21° so it does not chatter at 20°: if the distance followed every small change, every texel would change size every frame and shadow edges would shimmer, and no cascade could stay cached. Third, only the far end is fitted. Starting the cuts at the nearest surface rather than at the camera sounds tidier, but logarithmic cuts then all move outwards, and the ground right at your feet ends up in a bigger, coarser box.

Finer texels cost a little: the measurement takes about 0.02 ms, and where a penumbra that used to be smaller than one texel now spans several, the soft shadow's samples spread wider in the atlas (about 0.1 ms more in the room at 2560x1440).

**What comes later.** Texels a little coarser than pixels still show faint stair steps at an edge; temporal anti-aliasing smooths those, and ray-traced sun shadows (phase 6d, an optional setting) avoid texels altogether. Unreal Engine 5's **virtual shadow maps**, one huge sparse shadow map whose pages are drawn only where the screen needs them, are on the roadmap's list of later ideas.

### More lights than seats

A light the shader uses has a **seat** in its light buffers. Before clustered shading every pixel looped over every seated light, so the number of seats was a cost dial, 64 point lights and 8 spotlights. Now a light costs only where it shines (section 8, Clustered shading), so by default every light the buffers hold is seated: 1024 point lights and 256 spotlights. Shadows are still scarce: 64 point lights and 8 spotlights at most, the most the shadow atlas holds (a game may set fewer). A level with a torch every few metres has more lights than that, so each frame `prioritizeLights` (`render/gpu/light_priority`) decides who gets a seat and, above all, who gets a shadow. Picking the first lights in the list would be arbitrary, and worse, unstable: removing a component moves the last one in its storage into the hole (section 4), so switching the flashlight off could hand a shadow to some other light across the map.

The rules:

1. **Out of view, out of the running.** A point light or spotlight cannot light anything beyond its `range`. If that whole sphere is outside the camera's frustum, it cannot change a single visible pixel, so it takes no seat. This is the same sphere test frustum culling uses for meshes.
2. **Ranked by brightness at the focus.** Each remaining light scores how brightly it lights one point, the **focus**: `intensity × (brightest colour channel) / (d² + r²)`, the inverse-square falloff the shader uses. Physically it is the irradiance the light delivers there. The top lights get the seats (all of them, unless there are more than 1024 point lights or 256 spotlights in view, or a game sets fewer seats) and the top 64 point lights and 8 spotlights the shadows; directional lights have no position and are ranked by brightness alone. The shader's range window is left out of the score on purpose: it is exactly zero beyond the range, so every light out of reach of the focus would score zero and tie, and ties fall back to the arbitrary order. Without it, far lights still rank by how far they are.
3. **Shadows to the top casters.** Of the seated lights with `castsShadows`, the most important get shadows, up to a cap. The engine sets the caps to what the atlas holds (64 point lights, 8 spotlights), since shadow caching makes lights that stand still nearly free; a game could lower them (`LightPrioritySettings`), and then the rest would still light the scene, unshadowed.
4. **Hysteresis.** Walk between two similar torches and their scores cross. With a plain "top 64", the seat would flick from one to the other, and back and forth every frame while you stand near the midpoint. So a light that had a seat or a shadow last frame counts 1.25 times as important (`incumbentAdvantage`). A rival must be clearly brighter to take it, about 11% closer for an equal light. It is how a thermostat that switches on at 19° and off at 21° avoids chattering around 20°, or a Schmitt trigger in electronics. To know who had what, each collected light carries its `Entity`, and the renderer keeps last frame's choices in a `LightHistory`.
5. **Fading.** Hysteresis stops flicker, but a handover still happens in one frame: a pool of light or a pillar's shadow simply vanishes. So handovers fade instead, over a quarter of a second. The light losing its seat (or its shadow) keeps it while it fades out, and only when it reaches zero does the winner get it and fade in. That way the limits always hold: there are never more seated or shadowed lights than allowed, even mid-handover. Two cases are deliberately not faded. A light that was not on screen last frame (just switched on, or just come into view) starts at full strength, because nothing showed it before, so nothing can pop; pressing F switches the flashlight on at once. And a light whose range leaves the view is dropped at once, because it was lighting nothing you could see. The strengths reach the shader cheaply: a light's fade is multiplied into its colour on the CPU, and a shadow's strength blends between shadowed and fully lit.
6. **Manual priority.** Brightness is a good guess, but only the game knows that the boss's aura or the lamp on the objective must never go dark. Point lights and spotlights have an integer `priority` (0 by default): a higher priority always wins a seat and a shadow, and brightness and hysteresis only decide between lights of equal priority. It does not bring back a light that is out of view, since that one cannot change a pixel anyway.

**Where is the focus?** For a first-person view the camera is down among the lights, so its position works (the default, `Game::getLightFocus`). A MOBA camera floats about 16 units from the ground it shows, further than a torch reaches: measured there, every torch on screen would score almost nothing, and a dim lamp near the camera would beat them all. So the demo's MOBA mode passes the ground point in the middle of the screen (`MobaCameraController::getLookPoint`). It is like judging street lamps by how they light the street, not the satellite photographing it. The game supplies the point because only it knows where the action is; it reaches the renderer as `FrameDescription::lightFocus`.

To watch it work, press **L** in the demo: it adds 100 coloured lamps, each flying round a stone pillar like a firefly (a `Firefly` component: a circle whose radius, height and speed wobble as sums of sines with mismatched frequencies, like a Lissajous figure, so the path never quite repeats). The pillar shadows sweep round like sundial hands, and since a moving light invalidates its cached shadow tiles, this is also the worst case for shadow caching: the F4 readout shows over 200 tiles redrawn every frame. Every bulb glows (that is its emissive material, not its light). That is more than the 64 point light shadows, so when most of the field is in view, light priority has to choose: every lamp lights the ground, the ones nearest the focus also shadow their pillars, and as you move, shadows fade from lamp to lamp. (Before clustered shading only 64 lamps could even light the ground, and the far rows stayed dark.) (Before shadow caching only 16 lamps could have shadows, and the shadows visibly followed you around.) The F4 readout counts the lamps in view, lit and shadowed.

### Objects in the sky

The real moon is so far away that it shows no **parallax**: walk a hundred metres and it is still in the same direction and the same size. A sky object that should behave like that is kept at a fixed offset from the camera every frame, so its direction and size never change; that is the trick skyboxes use. Its light would be a `DirectionalLight`, with the same direction everywhere.

The demo deliberately does the opposite for the moon sphere: it is a fixed object 64 units from where the free camera starts, so you can fly to it in F3 mode in about 20 seconds. As you move, it shifts and grows like any nearby object, while the moonlight, a `DirectionalLight`, keeps the same direction everywhere, so up close the sphere is no longer exactly where the light seems to come from. (For a while the moonlight was a `PointLight` inside the sphere, which made the light really come from it, but a lamp 64 units away dims across the scene, needs a huge intensity, and gets blurry shadows.)

The light lab's sun is drawn that way. Each frame its disc, a glowing sphere, is placed at 90% of the camera's far plane straight along the way to the sun (`skyObjectPose`), and scaled so it spans the sun's real angular radius, 0.267° either side of its centre (a sphere of radius r seen from d away spans asin(r/d), so r = d sin θ). That makes it about 11 pixels across on a 1440p screen, as small as the real one looks; the Sun disc size setting enlarges it for looks without changing the shadows. It casts no shadow, since it stands for the light's own source.

How bright it glows follows from the light it gives. A disc of radiance L covering a patch of sky of solid angle Ω delivers an illuminance E = L Ω to a surface facing it, so L = E / Ω, where Ω = 4π sin²(θ/2), about π θ² for a small disc (`discSolidAngle`, written with the half angle because 1 − cos θ loses most of its digits in floats when θ is tiny). The engine's light values already include Lambert's 1/π (a white wall facing a light of value v shows v, not v/π), so the illuminance is π v and the disc's radiance π v / Ω (`discRadiance`). For the real sun that is about 1/θ², some 46,000 times as bright as a white wall it lights, which is why you cannot look at it. A bigger disc glows dimmer by exactly as much, so the light in total stays the same, and at sunset the disc reddens and dims with the light itself. One concession: full sunlight at intensity 3 asks for about 140,000, more than the half-precision floats the scene is drawn into can hold (65,504), and the overflow would turn into infinities spreading through bloom. So the radiance is capped at 30,000 by scaling all three channels together, which keeps its colour. It still burns to pure white through tone mapping, and bloom spreads a glow around it as a camera lens would.

### Where the sun stands: a sundial in reverse

The light lab places its sun with the same spherical trigonometry a sundial uses, so one "time of day" slider moves it along a real arc: low in the east in the morning, highest at noon, setting in the west, higher in summer and lower towards the poles.

Picture Earth from outside: it spins once a day about an axis tilted 23.44° from its orbit. Seen from the ground, that spin makes the sun circle the **celestial pole** once a day, and the tilt sets how far the circle sits from the celestial equator. That angle is the sun's **declination** δ: +23.44° at the June solstice, -23.44° at the December one, 0 at the equinoxes. Over the year it follows a cosine closely enough for lighting, δ ≈ -23.44° · cos(2π (day - 355) / 365). How far Earth has turned since local noon is the **hour angle** H, 15° an hour (360° in 24 hours). And where you stand is your **latitude** φ.

The sun's direction is then one rotation away. In Earth's turning frame it points at (cos δ cos H, cos δ sin H, sin δ); tilting that by your latitude into local east, north and up gives

- east = -cos δ sin H (negative after noon: the sun is in the west)
- north = sin δ cos φ - cos δ cos H sin φ
- up = sin δ sin φ + cos δ cos H cos φ, the sine of the sun's height above the horizon.

Squared and added, these give 1, so the direction comes out as a unit vector with no normalising. Two checks you can do in your head: at noon (H = 0) the height is 90° - |φ - δ|, so at the equator at an equinox the sun is straight overhead; and at the north pole (φ = 90°) the height is δ all day long, the midnight sun. The tests check exactly these, plus sunrise and sunset where the height is zero, at cos H = -tan φ tan δ.

Two simplifications, both too small to see in lighting: the clock is local *solar* time (12:00 is when the sun crosses the meridian, which differs from a wall clock by the time zone and up to 16 minutes over the year, the "equation of time"), and the atmosphere's refraction, which lifts the sun about half a degree near the horizon, is left out.

The time of day is a setting that the ticks move on (0 hours per second by default, so the sun holds still), so it depends only on how long the game has run, never on the frame rate. From the sun's height the light lab picks the day's light by eye until a real sky arrives in phase 6d: sunlight fading in over the last degrees of sunrise and reddening below about 30° (low light crosses more air, which scatters the blue out of it, the same scattering that makes the sky blue), and a blue sky that dims through dusk. The background is a `SkyBackground` component: the colour the renderer clears the picture to before drawing, so it shows wherever nothing is drawn. By itself it lights nothing, but the light probes' rays that escape to the sky bring its colour back, so through them it lights the level. The light lab's flat ambient light is set to the same value: a uniform sky drawn at brightness B gives a surface facing it exactly the irradiance B (the next section shows why the units work out that way), so the flat ambient where there are no probes matches what the probes measure.

### Global illumination: light that bounces

Everything so far is **direct** light: from a light straight to a surface, then to the eye. Real light keeps going. A sunbeam through a window lands on the floor, and the floor sends a fifth or so of it back up, onto the walls and ceiling, which send some on again. That is why a room lit through one window is not black everywhere the beam misses. The flat ambient term was a stand-in for all of that: the same light from every direction, everywhere, inside a closed room as much as on an open field.

**What a surface needs.** A matte surface's brightness depends on its **irradiance** E: the light arriving from the whole hemisphere above it, each direction weighted by the cosine of its angle to the normal (Lambert's law again). Irradiance changes slowly from place to place and with the normal's direction, like a temperature field, so it can be measured at a coarse grid of points and interpolated in between. Those points are **light probes**, and the method here is DDGI ("dynamic diffuse global illumination", Majercik et al. 2019, as rebuilt for production in 2021).

**What a probe stores.** Each probe keeps two tiny pictures of the whole sphere of directions, unfolded flat by the octahedral mapping (squash the sphere onto an octahedron, look down on it, fold the bottom half out over the corners: one square, nearly even texels, easy borders):
- an 8 x 8 picture of the irradiance arriving from around each direction;
- a 16 x 16 picture of how far away the nearest surface is in each direction, as its mean and its mean square.

**How probes learn.** Every frame each probe shoots 64 rays in a spiral of directions (a Fibonacci spiral, turned by a random rotation each frame so the gaps move and, over time, cover everything). Hardware ray tracing says what each ray hits. A ray that escapes brings back the sky; a ray that hits a lit surface brings back what that surface sends towards the probe: its glow, plus its colour times the light reaching it, the sun and the few brightest nearby lamps, each checked by a shadow ray. The probe blends the 64 answers into its pictures. It does not replace them: it keeps most of the old value and mixes in a little of the new, like a capacitor charging through a resistor, an exponential relaxation with a time constant (0.5 s for irradiance), so the light settles smoothly and the same at any frame rate.

The blend is a plain average of the light itself. The 2021 paper stores the fifth root of the light and averages that, so that a light switched off fades fast; but the average of roots is below the root of the average (Jensen's inequality, the same reason the mean of square roots of a set of numbers is below the square root of their mean), and in a room lit by a few rays that happen to hit a bright patch the error is large: such a probe settled at half its true light. What is left is plain counting noise. A probe's light is an estimate from a few dozen random rays, so it wobbles like any Monte Carlo average, by about 1/√(rays per frame × frames of history): four times the rays or four times the time constant halve it. Dark rooms show it most, since their light comes from the few rays that find something bright.

**Infinite bounces for the price of one.** The surface a ray hits is itself lit by bounced light, which the probes measured last frame. So each hit also adds its colour times the probes' irradiance there. This frame's probes then hold one bounce more than last frame's, and a still scene gathers bounce after bounce, a geometric series E_direct (1 + ρ + ρ² + ...) with ρ the surfaces' albedo, one term per frame, which converges because ρ is below 1. A setting (`bounceEnergy`, 0.85) passes on slightly less than all of it, which hides tiny leaks that repeated bounces would otherwise amplify, and the eye notices those most in dark rooms.

**Reading the probes at a pixel.** A pixel sits in a box (a grid cell) with a probe at each of its 8 corners. Each corner probe's irradiance, read in the direction of the pixel's normal, is weighted by three things:
- **Nearness:** trilinear weights, the 3D version of interpolating along a line. The 8 weights add up to 1 and reproduce any field that changes linearly.
- **Which side it is on:** a probe behind the surface counts less, through a soft curve ((cos + 1)/2)² + 0.2 rather than a hard cut, so a small object in a room still has probes that count.
- **Whether it can see the point at all:** the leak-stopper. The probe knows, in the direction of the point, the mean distance μ to the nearest surface and its spread σ² = (mean square) - μ². If the point is nearer than μ, the probe sees it. If it is farther, Chebyshev's inequality (its one-sided form) bounds the chance that a ray from the probe gets that far: σ² / (σ² + (r - μ)²). A probe on the far side of a wall sees the wall well before the point, the bound is tiny, and that probe's light is all but switched off. This is the same statistics as variance shadow maps.

The point tested for visibility is first moved off the surface, mostly towards the viewer (the **self-shadow bias**): a point on a wall sits exactly where the probes' rays stop, which is where the test is least sure. Weights that end up very small are shrunk further (cubed below 0.2), because a faint wrong glow in a dark corner is what the eye catches first. The weighted average is taken of square roots of the stored values and squared at the end, which gives a single bright probe less pull.

**Diffuse and specular.** Probes measure light for matte surfaces. The shiny part of a surface (its specular reflection) properly needs reflections, which come later; until then it reads the same probes in the mirror direction, a reflection blurred as if the surface were completely rough. Without it, a varnished floor indoors reflected the bright flat sky value and glowed blue in a room that cannot see the sky.

**Where the probes stop.** A game places one box of probes (a `ProbeVolume`); the light lab puts 12 x 5 x 16 probes 1 m apart round its building. Past the box's edge the probes' light fades over half a cell into the flat ambient, so there is no seam. Frames without probes are drawn exactly as before: probe sampling is compiled into separate pipelines (a specialization constant), because even skipped code changed how the driver compiled the rest, and moved a few pixels.

**Units, with no fudge factors.** Pixel values in this engine are π times the physical radiance, and a light's value is the irradiance it gives a surface facing it. A probe averages the pixel values its rays bring back, with cosine weights, and divides by the sum of the weights; the average of L cos θ over the hemisphere, divided by the average of cos θ, is E / π, and the π in the pixel values cancels it. So the stored number is the irradiance itself and drops straight into the slot the flat ambient had. A probe under a uniform sky drawn at B stores B (the furnace test).

---

## 10. Collision and movement

Phase 5 gave the engine its first physics: knowing what a ray or a body touches, and moving a character through a level without passing through walls. Everything here lives in `physics/` (with a little in `math/` and `scene/`) and runs in the fixed-step simulation, so it works headless and in tests.

### Shapes as cores plus radii

There are three shapes (`physics/shapes`): a `Sphere` (centre and radius), a `Capsule` (a line segment `a`-`b` grown by a radius, like a pill; with `a == b` it is a sphere) and an `Aabb`, an axis-aligned box (its faces line up with x, y and z, so it is stored as a minimum and a maximum corner). `Shape` is a `std::variant` of the three: one value that holds exactly one of them (see the programming notes below).

The key idea is that a rounded shape is a **core** (a point for a sphere, a segment for a capsule) **plus a radius**. Two rounded shapes overlap exactly when their cores come closer than the sum of their radii. So every overlap test between rounded shapes reduces to one geometry question: how close do two cores get? Think of the radius as a sheath of constant thickness round the core; the sheaths touch when the cores are within the two thicknesses of each other.

Those "closest points" questions live in `math/closest_points`:

- **Point to segment.** Project the point onto the segment's line, `t = (p - a)·d / (d·d)` with `d = b - a`, and clamp `t` to 0..1 so the answer stays on the segment.
- **Point to box.** Clamp each coordinate to the box's range. A point inside stays where it is (distance 0).
- **Segment to segment.** Two points `a1 + s d1` and `a2 + t d2`; the squared distance between them is a quadratic in `s` and `t`, a bowl-shaped surface. Its lowest point comes from setting both partial derivatives to zero (two linear equations), then clamping to the square 0..1 and re-solving along the edge it was clamped to. Parallel segments make the equations degenerate and are handled separately (Ericson, *Real-Time Collision Detection*, section 5.1.9).
- **Segment to box.** The squared distance from a point moving along the segment to the box is a smooth bowl as well, but it changes formula where the point crosses one of the box's six planes (on each axis the point is below, inside or above the box's range, and only the axes outside count). So the segment is cut at those crossings, and on each piece the distance is an ordinary quadratic, minimised exactly. No iteration, and a segment passing through the box comes out at distance 0.

**Touching counts as overlapping** (distance equal to the summed radii). The choice matters at the boundary: a character standing exactly on the floor must count as on it.

**Squared distances.** Comparing `distance² < (r1 + r2)²` gives the same answer as comparing distances, without a square root. Most tests in the engine work with squares for that reason.

### Raycasts

A **ray** is a starting point and a direction, `p(t) = origin + t × direction`; a raycast asks for the first `t ≥ 0` where it meets a shape (`physics/raycast`). The result is a `RayHit`: the distance (in world units, since the direction is normalised first), the point, and the surface normal, always turned to face back against the ray, so shading or a bounce can use it without checking which side was hit. Starting inside a shape hits at distance 0 with the normal pointing back along the ray: you are already touching it.

- **Sphere:** substitute the ray into `|p - c|² = r²` and you get a quadratic in `t`. A negative discriminant misses; otherwise the smaller root is where the ray enters.
- **Capsule:** the side is a cylinder, which becomes a circle once motion along the axis is ignored: remove the axis component from both the ray direction and the offset, and solve the same quadratic in the remaining 2D plane. That hit only counts if it lands between the segment's ends; the ray is also tested against the two end spheres, and the first of these hits is the answer.
- **Box (the slab method):** the box is where three slabs overlap (x between its minimum and maximum, and the same for y and z). For each axis, work out the `t` at which the ray enters and leaves that slab. The ray is inside the box when it is inside all three, so it enters at the latest entry and leaves at the earliest exit; it hits if the entry comes before the exit. The face hit is on the axis with the latest entry.
- **Triangle (Möller-Trumbore):** solve `origin + t d = a + u (b - a) + v (c - a)` for `t`, `u` and `v` (three equations, three unknowns, solved with cross and dot products). The ray hits when `u ≥ 0`, `v ≥ 0` and `u + v ≤ 1` (the point lies inside the triangle) and `t ≥ 0`. Triangles are hit from both sides. One demo bug taught a lesson: a ray passing exactly along the seam between two triangles could slip through both, because rounding put it just outside each. So the inside test allows a tiny tolerance (`edgeTolerance`, 1e-5), and that exact ray is kept as a regression test.

A **mesh** raycast (`raycast(ray, TriangleMesh)`) tests its triangles and keeps the nearest hit. `TriangleMesh` (`physics/triangle_mesh`) is the collision form of a mesh: positions plus three indices per triangle, built from a scene mesh by `collisionMesh(mesh, transform)`.

### Colliders: shapes that belong to entities

A `Collider` component (`physics/collider`) gives an entity a shape in its **own** coordinates, plus an optional `detail`: the exact triangles, shared between entities with `std::shared_ptr`. The shape must enclose the triangles; it is the cheap stand-in tested first.

Since the shape is in the entity's coordinates, it has to be moved into the world before testing (`worldShape(shape, model)`), with the entity's model matrix (its `Transform`):

- **Points** (centres, segment ends) go through the matrix exactly.
- **Radii** are scaled by the largest stretch the matrix applies, the longest column of its 3x3 part. A sphere squashed into an ellipsoid is no longer a sphere, so the engine takes the sphere that encloses it: always safe, exact when the scale is uniform.
- **Boxes** turned by a rotation are no longer axis-aligned, so the world box is the axis-aligned box *around* the turned one. Its half-size on axis `i` is `Σ_j |A_ij| h_j`: each of the turned box's three half-edge vectors contributes its full reach along axis `i`, whichever way it points.

Games click on simple shapes rather than meshes because shapes are cheap and stable: a capsule round a character answers "did I click the duck?" in one quadratic, and a click that lands between two of the duck's feathers still counts.

### Two-stage picking

Clicking an object (`raycastColliders`, `physics/collider_raycast`) works in two stages. First the ray is tested against each collider's world shape. In `Triangles` mode, each shape hit is then confirmed against the entity's detail triangles; in `Shapes` mode the shape hit is the answer.

The skipping argument rests on one inequality. The shape encloses the triangles, so the ray cannot reach a triangle before it reaches the shape around it: **the shape's hit distance is a lower bound on the triangles' hit distance**. Once some triangle hit is known at distance `d`, any collider whose shape is hit beyond `d` cannot contain anything nearer, and its triangles are never tested. The reach of the query shrinks as nearer hits are found.

The triangles are stored in the entity's own coordinates, so the ray is carried into them rather than every triangle into the world. That needs the inverse of the model matrix. For a matrix that only rotates, scales and moves (`[A t; 0 1]`), the inverse is `[A⁻¹, -A⁻¹ t; 0 1]` (`affineInverse`, `math/mat4`), with `A⁻¹` from the same cofactors the normal matrix uses. A scaled object makes distances along the ray differ inside and out, so a hit distance is converted back by the world length per local length along the ray, and the normal comes back through the normal matrix (section 8). A ray that slips between the triangles (through a hole in a mesh, or past the duck's beak) goes on to whatever is behind.

### The broad phase: a grid that skips most of the world

Testing a ray against every collider costs time in proportion to the number of colliders. The **broad phase** finds the few that could possibly matter before the exact tests (the **narrow phase**) run.

The engine's broad phase is a **uniform grid** (`physics/uniform_grid`): space cut into cubes 2 units on a side (`CollisionSettings::gridCellSize`), like graph paper in 3D. Each item goes into every cell its bounding box touches. Only occupied cells are stored, in a hash map from cell coordinates to the items in it, so empty space costs nothing. Items too big for this (more than 64 cells, such as the ground) go on a separate large list that every query checks.

- **Region queries** look only in the cells a box touches. An item that spans several cells would be found several times, so each item remembers the number of the last query that saw it (**mailboxing**) and is reported once.
- **Ray walks** visit the cells along the ray in order, near to far, stepping from cell to cell by whichever cell boundary the ray crosses next (Amanatides and Woo's grid traversal, the 3D version of drawing a line on pixels). The walk stops as soon as the next cell boundary is further than the nearest hit found so far, because nothing in a further cell can beat it.

`CollisionScene` (`physics/collision_scene`) holds the grid for every `Collider`, and is **rebuilt from scratch** each time it is used. That sounds wasteful but is deliberate: the ECS has no change tracking, so a grid updated piece by piece could keep stale entries for things that moved or were destroyed, while a rebuild is always right and, at these sizes, cheap. Tests check every query against brute force on random scenes.

### Static level collision

Things that never move (the ground, the obstacle course) are a `StaticLevel` (`physics/static_level`): all their triangles merged into one world-space `TriangleMesh`, with its own uniform grid over triangle numbers. It is built once, as pieces are added, and never rebuilt.

It answers raycasts (the walk stops at the nearest triangle hit) and, for movement, **contacts**: for a capsule, every triangle it touches, each as a `Contact` with the point, the normal pointing out of the triangle towards the capsule, the depth of overlap and the triangle's own face normal. The depth is the radius minus the distance from the capsule's core to the triangle, found as closest points again (`closestPointOnTriangle` decides which feature is nearest, the face, an edge or a corner, by which region of the triangle's plane the point falls in, Ericson section 5.1.5). A core that pierces the triangle is pushed out along the face normal, on the side its middle lies.

Spheres can also be **swept**: `sweepSphere` finds the first touch of a moving sphere. A sphere's centre touches a triangle exactly when it reaches the triangle grown by the radius (two copies of the face, moved out along the normal, plus a capsule along each edge), so the sweep is one raycast of the centre's path against that grown shape. Nothing passes through a wall at any speed, which stepping alone cannot promise.

### The kinematic character controller

A real body pushed into a wall feels a contact force, decelerates and bounces a little. Game characters are usually **kinematic** instead: the code decides how they move, and collisions only correct the position. That gives precise, predictable control (stop on a coin, no sliding on slopes, no bouncing off walls), which players expect from an FPS. The controller (`physics/character_controller`) moves a capsule (radius 0.4, height 1.8) through the static level each tick, and was tuned to feel like Overwatch using community-measured numbers.

One tick, in order:

1. **Jump** if asked and on the ground: the vertical speed becomes 5.72 m/s. It comes first, so the take-off tick already uses air rules.
2. **Steer.** On the ground, the horizontal velocity moves towards the wished velocity at 100 m/s² (full speed in a twentieth of a second). In the air, wishing nothing keeps the momentum; steering moves towards the wish at 30 m/s², and a turn of 90° or less keeps the speed instead of losing it on the way round, which is how Overwatch's air control has worked since 2018.
3. **Fall.** Gravity is 17.5 m/s² (game feel, not 9.81), with quadratic air drag while falling: `a = -g + (g / v_t²) v²`. That is Newton's law for drag proportional to speed squared, and it approaches the terminal speed `v_t` = 30 m/s as `v(t) = v_t tanh(g t / v_t)`. It is integrated with **semi-implicit Euler**: update the velocity first, then move with the *new* velocity. Plain (explicit) Euler, which moves with the old velocity, slowly adds energy, so jumps would creep higher.
4. **Move along the ground.** While walking, the horizontal velocity is tilted into the ground's plane and scaled back to full length, so slopes are walked at the same speed as flat ground.
5. **Substeps.** The move is cut into steps of at most half the radius. A step longer than the capsule is thick could jump right over a thin wall, the classic **tunnelling** problem.
6. **Push out** after every substep: find the deepest overlap, push the capsule out of it, ask again (pushing out of one wall can push into another, as in a corner), up to 8 times. The push depends on what was hit:
   - **Ground** (a walkable face, up to 45°) pushes straight **up**, by `depth / normal.y`, the height that clears the same overlap. Pushing along the normal instead would slide the character downhill a little every tick; going straight up is the kinematic stand-in for static friction.
   - **Walls and ceilings** push along the normal and remove the velocity going into them. That is a frictionless, perfectly inelastic collision: the motion into the wall stops, the motion along it continues, so the character slides along walls.
   - **Steps.** When a round body bumps a ledge of height `h`, the contact comes from the ledge's edge, and the push points at an angle `acos((r - h) / r)` from up, often steeper than any walkable slope. So an *edge* contact no higher than `stepHeight` (0.3) above the feet also counts as ground and pushes up: the capsule rides up kerbs and stairs by itself, with no separate step-up move. A steep ramp is still a wall, because walking into it touches its *face*.
7. **Settle onto the ground.** A character that was walking is lowered up to 0.3 units in small drops, so it follows the ground down a step or over the top of a ramp instead of launching into the air, and any hovering left by a push-out disappears.
8. **Look underfoot.** A capsule lowered by 0.01 that touches supporting ground decides `grounded` and the ground's normal.

The **first-person body** (`gameplay/first_person_body`) is an invisible capsule with this controller. Each frame, WASD becomes a wished velocity turned by the camera's yaw: 5.5 m/s, 90% backwards, 3 m/s crouched, 1.5 times sprinting. Crouching shortens the capsule at once; standing up waits until the full capsule fits. The camera sits at the body's eyes. The MOBA duck walks with the same controller, steered by its click-to-move target.

### Particles: balls that bounce

A `Particle` (`physics/particle`) is a small ball that flies and bounces off the level: the demo throws them as grenades with G.

- **Flight** under constant gravity moves by the average of the starting and ending velocities, `Δx = v t + ½ a t²`. That is the exact solution for constant acceleration, so a throw follows the true parabola however long the tick.
- **Collisions are swept, not stepped:** each tick the flight is swept through the level with `sweepSphere`, and at the first surface the ball stops just clear of it, bounces, and flies on for the rest of the tick (up to 4 surfaces per tick).
- **A bounce** splits the velocity into the part into the surface and the part along it. The normal part comes back `e` times as fast (the **coefficient of restitution**, 0.45: 0 sticks, 1 bounces fully). Coulomb friction then slows the sliding part by at most `μ` (0.4) times the change in the normal speed, and never reverses it. For a ball resting on a floor this gives exactly the kinetic-friction deceleration `μ g`, as in the textbook block on a table.
- **Coming to rest.** Arriving slower than 1 m/s does not bounce, so the bouncing dies out instead of halving forever (Zeno's ball), and a ball in contact with the ground has gravity cancelled by the surface. A resting ball stays exactly still: no jitter.

### Measuring speed: the profiler and the release build

Physics is the first work whose cost grows with the scene, so phase 5 added a way to measure it. `Profiler` (`core/profiler`) times named sections with `std::chrono::steady_clock`, a clock that only goes forwards at a constant rate (unlike the wall clock, which jumps when the computer corrects its time). It counts **per call**, not per frame, because ticks do not fall one per frame: at 240 frames a second and 120 ticks, half the frames run no tick at all. And it collects for half a second before showing anything, then shows that window's average and its worst call: readable digits, and the occasional slow frame (a hitch you feel) is not averaged away. F4 shows the engine's frame, work (the frame minus the frame cap's sleep), tick, update, scene and render times, and what each demo system costs per tick.

Those numbers showed why the **release build** matters. The normal build compiles with no optimisation (`-O0`), so a debugger can stop on every line and see every variable. But then every variable lives in memory, nothing is simplified, and every function call really happens. A line like `velocity + acceleration * seconds` becomes two function calls, each copying three floats in and out, to do six multiplications and additions. `make release` compiles with `-O2`: values stay in registers, repeated work is removed, and small functions are **inlined** (their body is pasted where they are called). For that, the compiler must *see* the body, so the small vector operations are defined in their headers (`math/vec3.h` and friends); a body in a separate `.cpp` file is invisible when compiling the files that use it.

Measured with 40 characters walking a stepped level and 200 bouncing balls, per tick:

| Build | 40 characters | 200 balls |
|---|---|---|
| Debug (`-O0`) | 0.52 ms | 0.54 ms |
| `-O2`, vector operations in `.cpp` files | 0.105 ms | 0.090 ms |
| `-O2`, vector operations in headers | 0.075 ms | 0.067 ms |

Seven to eight times faster in all, of which the headers gave 1.4 times: that part was the cost of the function calls themselves. Link-time optimisation (which inlines across files at link time instead) gained only another 3 to 9% and made a one-file rebuild take 20 seconds instead of 3, so it is not used.

### Programming notes met along the way

- **`std::optional<T>`**: a value that may be missing. A raycast returns `std::optional<RayHit>`: either a hit or nothing, with no magic distance like -1 meaning "missed". `if (hit)` checks, `*hit` or `hit->distance` reads.
- **`std::variant` and `std::visit`**: `Shape` holds a `Sphere`, a `Capsule` or an `Aabb`, and knows which. `std::visit` calls the right function for whatever it holds, so `overlaps(Shape, Shape)` is one line that picks among the nine pair tests.
- **Overloading**: one name, several functions told apart by their argument types. `raycast(ray, sphere)`, `raycast(ray, capsule)` and `raycast(ray, mesh)` read as one idea, and the compiler picks.
- **`std::shared_ptr`**: shared ownership. A collider's detail mesh is held by every entity that uses it (the demo's cubes and pillars all share one), and freed when the last one lets go. The `const` in `shared_ptr<const TriangleMesh>` stops any one of them from changing it under the others.
- **Floating-point tolerances**: floats carry about 7 significant digits, so results that should be exactly equal often differ in the last bit. The physics compares against small named tolerances (`edgeTolerance`, `clearance`, `touchingDepth`) instead of testing `==`, and keeps a ball exactly `clearance` (1 mm) off a surface it stops at, so rounding cannot leave it inside.
- **RAII for timing**: `ScopedTimer` reads the clock when made and records the difference when destroyed. C++ always runs destructors when a block ends, however it ends, so the stopwatch can never be left running: the same idea the engine uses to free SDL resources. `timeSection(profiler, section, [&] { ... })` wraps that around a **lambda**, a small unnamed function written in place.

---

## 11. Testing

Tests are plain `assert`-based functions, one file per area in `tests/`, registered in `tests/test_main.cpp`. They need **no GPU and no window**, which shapes the design: anything worth testing is pulled out into pure functions (`collectLighting`, `packLighting`, `packMaterial`, `normalMatrix`, `toGpuDepthRange`, `generateSmoothNormals`), and the GPU code just moves their results into buffers.

New features are written **tests first**: write the failing test, watch it fail, then write the code. What cannot be unit tested without a GPU is checked by the GPU test tier (the shaders' maths against its C++ mirrors), by golden images (whole pictures), and by eye in the running demo.

**Golden images** are the renderer's reference photographs. The demo lists six fixed views (the start, also in HDR, the normal map gallery, the lamp field, the floodlights, the moon), each set up in a fresh game with default settings and simulated to a fixed moment, so it is the same scene every time. `make golden-save` renders each off screen at 1280x720 and saves two pictures: the scene before tone mapping (half floats, packed losslessly into an ordinary PNG) and the final image. `make golden` renders them again and compares every pixel. On the same GPU, driver and shaders the result is bit for bit identical, so any difference at all means something changed; a heatmap of the differences shows where. It needs a GPU, so it is separate from `make test`. It is what made the Vulkan port safe: twelve pictures matching to the last bit is far stronger evidence than "looks the same to me".

**The GPU test tier** closes a gap the other two leave. The shading maths exists twice: GLSL that the GPU runs, and a C++ copy (a "mirror") that the unit tests check against the physics, for example that energy is conserved, Fresnel reaches 1 at grazing angles, and a white surface passes the furnace test. Nothing tied the two copies together except care when editing. A typo in the shader that the C++ copy does not share would show up only as a slightly wrong picture, and the golden images would say "this picture changed" without saying which function was wrong.

So the shaders' maths lives in small libraries of **pure functions** (`brdf.glsl`, `tone_mapping.glsl`, `lighting.glsl` and five more in `assets/shaders/`). A pure function's result depends only on its arguments, like a mathematical function. It reads no textures and doesn't use the pixel's position, because a compute shader has neither. The passes `#include` these libraries, and so do small compute shaders in `tests/gpu/shaders/`. Those run the very same functions on thousands of fixed inputs: a grid of roughnesses and angles for the BRDF, brightness from black to a thousand times white for tone mapping, every edge and corner of the cube for the shadow face. The results come back and are compared with the C++ mirror on the same inputs. `make gputest` runs this and then the golden images. Like them, it needs a GPU, so it stays out of `make test`.

**Why "equal" is the wrong test.** The GPU will not produce the same bits as the CPU, for honest reasons:

- It **fuses** a multiply and an add into one operation with one rounding (FMA). That is more accurate, but different from the CPU, which rounds twice.
- Vulkan allows the built-in functions to be **slightly approximate**: `inversesqrt` may be 2 units in the last place off, division 2.5, and `pow` is built from `exp2` and `log2`, so its error grows with the exponent. A **unit in the last place (ULP)** is the gap between a float and the next one: about 1.2 × 10⁻⁷ of the value.

So two results count as equal when they differ by at most `absolute + relative × |expected|`. The absolute part matters near zero, where a relative test fails: 0 and 10⁻³⁰ differ by "100%". The relative part matters for large values, where a fixed difference would be smaller than one float step. What makes a tight tolerance safe is the size of the **gap** between the two kinds of difference. Rounding lands around 10⁻⁷ to 10⁻⁵ relative. A real bug (a wrong constant, swapped arguments, a missing π) is off by percent or more. The report prints, for each function, how much of its tolerance the worst value used. On the RTX 5080 nearly everything uses under 16%. With two bugs planted on purpose (Fresnel's fifth power made a fourth, the light falloff's fourth power made a cube), the same test failed at 4,000 to 27,000 times the tolerance.

**When the problem itself amplifies rounding.** One comparison failed at first, and the reason is worth knowing. A near-mirror's highlight is a tall, narrow spike: GGX is α² / (π d²) with d = 1 − (n·h)²(1 − α²), and near the peak d is a small difference of two numbers close to 1. Both sides compute the halfway vector h their own way, so n·h can differ by a float step. A change δ in n·h changes d by about 2δ, and so the brightness by about 4δ / d relative. For a fairly shiny surface (α = 0.03) seen 10⁻⁴ from the peak, d is about 10⁻³, and one float step already moves the result by 2 × 10⁻⁴. This is the **condition number** of the problem: how much a relative error in the input grows in the output. Physicists know it as a sensitive dependence on initial conditions: a well-aimed experiment in a region where the answer swings wildly. The fix is not a looser tolerance everywhere. The specular comparison's allowance grows with 1 / d, so it is generous exactly where the maths is touchy and stays tight elsewhere. Diffuse light has no such spike, and keeps the plain tolerance.

The noise that turns soft shadow samples has a similar story. It keeps only the fractional part of a large product, so across a 4K screen its inputs keep only a few digits, and its results may differ by about 2 × 10⁻³. That is invisible as grain, but not rounding-sized, so it has its own tolerance. It also wraps around at 1, so 0.9999 and 0.0001 count as neighbours.

What the GPU tests leave alone is **texture filtering** (bloom's blur, the soft shadow sampling, the BRDF table lookup). How hardware blends neighbouring texels is not something a table of numbers checks cleanly, and the golden images keep covering it.

The **software renderer** (`render/software/`) is the engine's first renderer, drawing pixels on the CPU. It is kept as a tested reference for rasterising, clipping and depth testing. It does flat shading only and still has its own fixed light.
