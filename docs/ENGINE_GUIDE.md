# Engine guide

How the engine works and why it is built this way. This is for people learning the engine, so it explains concepts as well as code. For the exact API, read the headers; for what comes next, see [ROADMAP.md](../ROADMAP.md).

This guide describes the engine as it is now. It is updated at the end of each roadmap phase and after any large feature.

**Covers:** phases 0 to 5 and phase 6's physically based materials and quaternions (quaternions and slerp; collision shapes, raycasts, colliders and picking, the broad phase grid, static level collision, the character controller, particles, the profiler and the release build; lights including spotlights, emissive materials and the moon, physically based materials (microfacets, Fresnel, metals, multiple-scattering energy compensation, the white furnace test), smooth normals, normal matrix and depth range, mipmaps, gamma-correct colour and dithering, the frame description, shadow mapping, debug drawing, frustum culling, light priority, shadow face skipping, shadow caching and soft shadows).

---

## 1. The big picture

The engine is meant to run two very different kinds of game, an FPS and a MOBA. So the core knows nothing about either: no weapons, no abilities, no "player". Genre-specific code sits on top, in `gameplay/` and in each game's own folder under `games/`.

The source is split into layers, and **a layer may only use the layers below it**:

```
app/            picks a game and starts the engine
games/          one folder per game (games/demo)
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

At start-up, `main` creates a `DemoGame` and an `Application` (the engine). The engine owns the window, the renderer, the input snapshot, the `World` of entities and the `AssetManager`, and drives the game through a few hooks.

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
- **`IndexedMesh`** is the *render* format: a list of unique vertices (position, normal, UV, exactly the GPU's layout) and 32-bit indices into it. Corners that are identical in every component are merged, so shared vertices are stored and processed once.

### What a normal is

A **normal** is the direction a surface faces, and lighting is computed from it. A file may give a normal for each corner. If it does not, the corner gets its triangle's **face normal** (from the cross product of two edges), and the model looks faceted: every triangle has one flat shade.

### Smooth normals (opt-in)

Curved models without normals, like the teapot, look blocky. `generateSmoothNormals(mesh, creaseAngle)` estimates the curved surface the triangles approximate:

- For each corner, average the normals of the faces around that **position** (by position, not vertex index, because some files duplicate vertices along seams; the teapot has 319 such duplicates).
- Weight each face by its **angle at that corner**, so the result depends on the shape of the surface, not on how it was cut into triangles.
- Only count faces within the **crease angle** (default 60 degrees) of the corner's own face. Gently curved areas get averaged; real edges (a cube's corners, a teapot's rim) stay sharp.

It is **opt-in** per load (`MeshLoadOptions{.smoothNormals = true}`), because only the game knows whether a shape is meant to be faceted: a stealth fighter's panels meet at shallow angles but should stay flat. Normals from the file are never replaced.

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

The renderer uses **SDL_GPU**, SDL3's portable GPU API, with the Vulkan backend. Shaders are written in GLSL and compiled to SPIR-V by `make`.

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

### Uniforms and why their layout is strict

Data that is the same for every vertex or pixel of a draw, such as matrices, material and lights, goes to shaders as **uniforms**. In SDL_GPU you *push* bytes into a numbered slot, and the shader reads them as a struct. Pushed data stays in effect for the rest of the frame, so lights and the camera are pushed once, and per-object data per draw.

The shader reads those bytes with **std140** layout rules, and C++ does not know them. The classic trap: a `vec3` takes 16 bytes in std140, not 12. To make mismatches impossible, every block uses only 4-component vectors, and each is mirrored by a C++ struct with a `static_assert` on its size (`LightUniformData`, 2736 bytes; `MaterialUniformData`, 48 bytes; `ShadowUniformData`, 896 bytes).

Uniform blocks are also small by design: SDL lets a shader read only the first 4 KB of each one, and anything past that silently reads as zero. (The engine learned this the hard way: when the shadow tile data grew past 4 KB, whole cube faces of shadow suddenly read as solid black.) Bigger data goes in a **storage buffer**, a plain GPU buffer the shader reads like an array, with no such limit; the per-tile shadow data (each tile's matrix and place in the atlas, 112 bytes per tile, up to 396 tiles, about 44 KB) lives in one, uploaded once per frame. Every uniform struct has a `static_assert` that it stays under 4 KB. If anyone changes one side, the build fails.

Matrices are transposed when pushed, because GLSL stores them column by column.

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

### Dithering: noise against banding

Even with gamma, the screen has only 256 levels per channel. A slow, dark fade, such as the flashlight's pool on the ground at night, changes by less than one level over many pixels, so whole patches round to the same level and then jump together: visible rings, or **banding**. It is quantisation, like measuring a gentle slope with a ruler marked only in whole centimetres.

**Dithering** adds a little noise before the rounding, up to half a level either way and different for every pixel. Near a boundary, some pixels then round up and some down, in proportion to where the true value lies between the two levels, and the eye averages them back to the in-between brightness (the same idea as halftone printing, or dither in audio mastering). The noise is sized in the screen's own steps, so the shader encodes to sRGB, adds it, and decodes again. It comes from a formula on the pixel's position (**interleaved gradient noise**), which spreads values evenly over every small patch, so the grain is fine and even. A value exactly on a level never moves, so flat colours stay clean, and on average nothing gets brighter or darker; `core/dither` tests both.

### Depth range

`Mat4::perspective` follows the OpenGL convention: depth runs from -1 at the near plane to +1 at the far plane. Vulkan wants 0 to 1. `toGpuDepthRange` fixes this inside the renderer (halve depth and add half of `w`), so nothing outside `render/` knows about it.

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

Each frame, `collectLighting` gathers lights from the `World` (point light positions interpolated like meshes, so a light carried by something moving does not jitter), and `packLighting` turns them into the shader's layout. The shader handles up to **4 directional, 64 point and 8 spotlights**; when a scene has more, the renderer picks the ones that matter most for the frame (see "More lights than seats", below).

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

Emissive surfaces do not light their surroundings: the moon's glow is just its own colour, and the actual moonlight comes from a separate `DirectionalLight`. The soft halo you expect around a bright light comes from a post-processing effect, bloom.

### The whole sum

```
colour = ambient · (diffuse colour + specular albedo) + Σ (diffuse + specular) per light + emissive
```

Light adds up linearly, like superposing intensities, so every light simply adds its share. A surface with no usable normal gets ambient and emissive light only. The whole sum happens in linear light (see gamma, above), into a floating-point image that tone mapping turns into the screen image afterwards.

### Shadows: a depth photo from the light

A point is in shadow when something sits between it and the light. Testing that directly for every pixel against every triangle is ray tracing. **Shadow mapping** gets the answer from one extra drawing of the scene instead:

1. Put a camera at the light and draw the scene keeping only depth. The result, the **shadow map**, records for every direction how far the nearest surface is from the light. It is like a photo taken with a rangefinder.
2. While drawing the camera's view, project each pixel's point into that photo and compare: if the photo saw something nearer to the light in that direction, something is in the way, so this light does not reach the point.

Each kind of light needs a different camera:

- A **spotlight** gets one perspective camera looking down its beam, just wide enough for its cone.
- A **point light** shines every way, so one photo is not enough: it takes six, one per face of a cube around the light, each a square 90 degree perspective camera looking along +X, -X, +Y, -Y, +Z or -Z. A direction belongs to the face of its largest component (`pointShadowFace`).
- A **directional light** (the sun) has parallel rays, so its camera has no perspective: it is a box (an **orthographic** projection), like photographing with light that never spreads. The sun lights the whole world, but a shadow map has finite resolution, so the box only covers the 30 units in front of the camera, and stretches 50 units back towards the sun so tall things outside the view still cast into it. As the camera moves, the box would slide by fractions of a texel and every shadow edge would crawl and shimmer; snapping its centre to whole texels (measured across the light's direction) makes it jump a texel at a time, so edges stay put.

All these views share one depth texture, an **atlas** of 8192 x 4096 texels (128 MB), because this SDL version cannot draw into one layer of a texture array. Each frame `planShadows` cuts it into square tiles: one per directional light, one per spotlight and six per point light, for up to 4 directional, 8 spot and 64 point lights: every point light seat in the shader can have a shadow.

Tiles come in sizes from 256 to 1024 pixels (2048 for spotlights), chosen by how far the light reaches (`shadowTileSizeFor`): enough texels that one covers about 0.04 units across the far end of the view (0.02 for spotlights). A lamp with a range of 5 has cube faces 10 units across at their far end, so 256 texels do; a floodlight with a 35 degree cone reaching 25 units is 35 units across there, so it gets 2048. Spotlights aim finer because their beams usually land on the ground at a slant well short of their reach, which stretches each texel along the ground (at 1024 the floodlights' shadows of steps and poles came out visibly jagged), and because there are few of them and they usually stand still, so their big cached tiles cost nothing to keep. A 256 tile holds a sixteenth of the texels of a 1024 one, so small lights are cheap, which is what makes 64 shadowed point lights fit. If the lights still ask for more than the atlas holds, the least important ones' tiles are halved until everything fits: point lights first, then spotlights, and directional boxes keep their 1024. The tiles are then packed like a **quadtree**: the atlas starts as 8 squares of 2048, a tile takes the smallest free square that fits, and a square that is too big is split into four quarters, as often as needed. Because every size is a power of two and the biggest tiles are placed first, the quarters of a split square always get used before another is split, so the atlas never ends up with unusable gaps. The shader is told each tile's rectangle in the atlas. The shadow pass draws the scene once per tile, all in one render pass, moving the viewport to each tile. Perspective views are made slightly wider than needed, so the samples around a point near a tile's edge stay on that tile. Models with `castsShadows = false` are left out of the shadow pass (the moon sphere, which surrounds its own light, would otherwise block everything), and the duck holds its flashlight out in front of it for the same reason.

The cost grows with every shadowed light: each tile is one more drawing of every shadow-casting mesh, so a shadowed point light costs up to six. That is why real games shadow only the lights that matter most, and why the cap exists.

Not every cube face is needed, though. A face can only shadow a pixel you can see if its view pyramid overlaps the camera's, and a lamp near you typically has one face pointing back past you at nothing on screen. `planShadows` tests each face with `Frustum::mayIntersect` and gives a face that fails no tile and no draw. The test is the same "all on the wrong side of one wall" idea as culling a sphere: a view is a convex shape, the space between its eight corners, so if all eight corners of one view lie outside one side of the other, the two cannot meet. It is checked both ways round, since a small view next to a corner of a big one is only caught by its own sides. Like the sphere test it can occasionally keep a face that just misses, but never drops one that is needed. A skipped face keeps its place in the list of tiles (with an empty rectangle), so the shader still finds face `f` of a light at its first tile plus `f`, and it counts anything there as lit.

Most lights and most of the world stand still, so most shadow tiles would come out exactly the same as last frame. **Shadow caching** keeps them: the atlas is not wiped between frames, and a tile is drawn again only when something it shows changed. Two pieces make that work:

- **Tiles keep their place.** `ShadowAtlasLayout` remembers which square each view had (named by its light's entity and its face) and hands the same square back while the view asks for the same size. Squares no longer wanted are given back, and four free quarters join up again into their parent square (a **buddy allocator**, after the paired halves in memory managers), so the atlas does not crumble into scattered small holes. If it ever does crumble so a new big tile has no room even though there is enough area in total, everything is packed afresh once, which always fits, and every tile is drawn again that frame.
- **The renderer remembers what it drew where.** For each square, `ShadowTileMemory` keeps the view matrix it was drawn with and a **hash** (a 64-bit fingerprint) of the casters inside it: their meshes and model matrices. Each frame each tile's casters are found and fingerprinted again, which is cheap next to drawing them; if the matrix and fingerprint match, the image already in the atlas is right and the tile is skipped. A light that moves changes its matrix; a box that moves, appears or vanishes in view changes the fingerprint. Drawing a tile forgets anything remembered in squares it overlaps, since it painted over them.

Since the atlas is kept, a tile that is redrawn must first be wiped by itself, and SDL can only clear a whole texture. So each redrawn tile first gets one big triangle at the far depth (1) drawn with the depth test switched off, which overwrites exactly that tile. When nothing needs drawing, the shadow pass is skipped completely. The F4 view shows how many tiles were drawn this frame, how many were kept from earlier frames, and how many cube faces were skipped.
Three practical problems, and their fixes:

- **Shadow acne.** Each texel of the shadow map stores one depth for a whole patch of surface. Tested against itself, a sloped surface comes out half in front of and half behind its own stored depth, in stripes. The fix is a small **bias**: the lookup nudges each point off its surface along its normal by 2.5 texels of its tile (more for points far from the light, where texels cover more ground), so it tests a point just above the surface. GPUs can also bias the shadow pass itself, pushing depths away from the light more on steep slopes, and the engine used to. But that slope-scaled push has no upper limit: a pillar's side seen almost edge-on from a lamp above it got pushed back behind the ground, and its shadow came loose from its base (**peter-panning**, after the boy whose shadow came off). Measured in texels, the normal offset stays in proportion with every tile size.
- **Jagged edges.** A shadow map has a finite resolution, so a plain in/out test draws staircase edges. The sampler does the comparison itself and blends the results of the four nearest texels, and the shader averages nine such lookups in a 3x3 grid (**percentage-closer filtering**, PCF): at an edge, the fraction of samples that are lit becomes a smooth gradient.
- **Soft shadows that look real.** A fixed blur gives every shadow the same edge, but real ones are sharp where an object touches the ground and blur further away. The reason is that lights glow over an area, not from a point. Behind an object's edge there is a band that sees part of the glowing area but not all of it, the **penumbra**, like the ring of partial eclipse around the moon's total shadow on Earth. By similar triangles its width is `emitter radius × (receiver distance − blocker distance) / blocker distance`: zero where the blocker touches the receiver, widening as the gap grows, and wider for bigger glowing areas. Point lights and spotlights get this (**percentage-closer soft shadows**, PCSS) in three steps per pixel: search the shadow map around the point for things nearer the light and average how far they are (this reads the atlas a second time as plain depths, since a comparison only says yes or no); work out the penumbra width from the formula; then average 16 depth comparisons spread over that width. The 16 points are a **Poisson disc** (spread out with no two close together, like seats taken in an empty cinema), turned by a different angle in every pixel so the leftover error looks like fine grain instead of bands. A wide filter brings back acne in a new form: on a surface sloping away from the light, a sample a few texels over is legitimately further from the light, and compared against the centre's depth it looks shadowed. So each sample is compared with the surface's own plane at that spot instead (**receiver plane depth bias**), found from how the surface position changes between neighbouring pixels. Each light has an `emitterRadius` for this, its real glowing size (0.05 for a bulb, 0.025 for a flashlight by default), kept separate from `sourceRadius`, which only shapes the falloff near the light and is tuned much larger. Directional lights keep the plain 3x3 filter: the moon's real penumbra would be narrower than one texel of its box.
- **Resolution.** Tiles are 256 to 1024 pixels across, sized by the light's reach. How much ground one texel covers depends on the view: the moon's box is 30 units wide on a 1024 tile, so about 0.03 units (a cube is 2 units across), sharp. A point light far from the scene is much worse: when the demo's moon was briefly a point light 64 units away, its 90 degree cube faces spread one texel over about 0.13 units, and most faces only saw empty sky. Faces outside the camera's view are now given back (above), but fitting point light views to where the scene actually is remains a possible improvement.

Real shadows from a large source are soft, because near an edge only part of the source is hidden (the **penumbra**, like the edge of the shadow in a solar eclipse), and the further the shadow falls from its caster, the softer it gets. The light's `sourceRadius` will drive that later (percentage-closer soft shadows).

### More lights than seats

The shader has a fixed number of **seats** (array entries) for each kind of light, and the shadow atlas has room for fewer still: 64 point lights can light the scene, and as many of them can cast shadows (a game may set fewer). A level with a torch every few metres has more lights than that, so each frame `prioritizeLights` (`render/gpu/light_priority`) decides who gets a seat and who gets a shadow. Picking the first lights in the list would be arbitrary, and worse, unstable: removing a component moves the last one in its storage into the hole (section 4), so switching the flashlight off could hand a shadow to some other light across the map.

The rules:

1. **Out of view, out of the running.** A point light or spotlight cannot light anything beyond its `range`. If that whole sphere is outside the camera's frustum, it cannot change a single visible pixel, so it takes no seat. This is the same sphere test frustum culling uses for meshes.
2. **Ranked by brightness at the focus.** Each remaining light scores how brightly it lights one point, the **focus**: `intensity × (brightest colour channel) / (d² + r²)`, the inverse-square falloff the shader uses. Physically it is the irradiance the light delivers there. The top 64 point lights and top 8 spotlights get seats; directional lights have no position and are ranked by brightness alone. The shader's range window is left out of the score on purpose: it is exactly zero beyond the range, so every light out of reach of the focus would score zero and tie, and ties fall back to the arbitrary order. Without it, far lights still rank by how far they are.
3. **Shadows to the top casters.** Of the seated lights with `castsShadows`, the most important get shadows, up to a cap. The engine sets the caps as high as the seats (64 point lights, 8 spotlights), since shadow caching makes lights that stand still nearly free; a game could lower them (`LightPrioritySettings`), and then the rest would still light the scene, unshadowed.
4. **Hysteresis.** Walk between two similar torches and their scores cross. With a plain "top 64", the seat would flick from one to the other, and back and forth every frame while you stand near the midpoint. So a light that had a seat or a shadow last frame counts 1.25 times as important (`incumbentAdvantage`). A rival must be clearly brighter to take it, about 11% closer for an equal light. It is how a thermostat that switches on at 19° and off at 21° avoids chattering around 20°, or a Schmitt trigger in electronics. To know who had what, each collected light carries its `Entity`, and the renderer keeps last frame's choices in a `LightHistory`.
5. **Fading.** Hysteresis stops flicker, but a handover still happens in one frame: a pool of light or a pillar's shadow simply vanishes. So handovers fade instead, over a quarter of a second. The light losing its seat (or its shadow) keeps it while it fades out, and only when it reaches zero does the winner get it and fade in. That way the limits always hold: there are never more than 64 lit or shadowed point lights, even mid-handover. Two cases are deliberately not faded. A light that was not on screen last frame (just switched on, or just come into view) starts at full strength, because nothing showed it before, so nothing can pop; pressing F switches the flashlight on at once. And a light whose range leaves the view is dropped at once, because it was lighting nothing you could see. The strengths reach the shader cheaply: a light's fade is multiplied into its colour on the CPU, and a shadow's strength blends between shadowed and fully lit.
6. **Manual priority.** Brightness is a good guess, but only the game knows that the boss's aura or the lamp on the objective must never go dark. Point lights and spotlights have an integer `priority` (0 by default): a higher priority always wins a seat and a shadow, and brightness and hysteresis only decide between lights of equal priority. It does not bring back a light that is out of view, since that one cannot change a pixel anyway.

**Where is the focus?** For a first-person view the camera is down among the lights, so its position works (the default, `Game::getLightFocus`). A MOBA camera floats about 16 units from the ground it shows, further than a torch reaches: measured there, every torch on screen would score almost nothing, and a dim lamp near the camera would beat them all. So the demo's MOBA mode passes the ground point in the middle of the screen (`MobaCameraController::getLookPoint`). It is like judging street lamps by how they light the street, not the satellite photographing it. The game supplies the point because only it knows where the action is; it reaches the renderer as `FrameDescription::lightFocus`.

To watch it work, press **L** in the demo: it adds 100 coloured lamps, each flying round a stone pillar like a firefly (a `Firefly` component: a circle whose radius, height and speed wobble as sums of sines with mismatched frequencies, like a Lissajous figure, so the path never quite repeats). The pillar shadows sweep round like sundial hands, and since a moving light invalidates its cached shadow tiles, this is also the worst case for shadow caching: the F4 readout shows over 200 tiles redrawn every frame. Every bulb glows (that is its emissive material, not its light). That is more than the 64 seats, so when most of the field is in view, light priority has to choose: the lamps nearest the focus light the ground and shadow their pillars, and as you move, seats and shadows fade from lamp to lamp. (Before shadow caching only 16 lamps could have shadows, and the shadows visibly followed you around.) The F4 readout counts the lamps in view, lit and shadowed.

### Objects in the sky

The real moon is so far away that it shows no **parallax**: walk a hundred metres and it is still in the same direction and the same size. A sky object that should behave like that is kept at a fixed offset from the camera every frame, so its direction and size never change; that is the trick skyboxes use. Its light would be a `DirectionalLight`, with the same direction everywhere.

The demo deliberately does the opposite for the sphere: it is a fixed object 64 units from where the free camera starts, so you can fly to it in F3 mode in about 20 seconds. As you move, it shifts and grows like any nearby object, while the moonlight, a `DirectionalLight`, keeps the same direction everywhere, so up close the sphere is no longer exactly where the light seems to come from. (For a while the moonlight was a `PointLight` inside the sphere, which made the light really come from it, but a lamp 64 units away dims across the scene, needs a huge intensity, and gets blurry shadows.)

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

New features are written **tests first**: write the failing test, watch it fail, then write the code. What cannot be unit tested (shaders, how things look) is checked by eye in the running demo.

The **software renderer** (`render/software/`) is the engine's first renderer, drawing pixels on the CPU. It is kept as a tested reference for rasterising, clipping and depth testing. It does flat shading only and still has its own fixed light.
