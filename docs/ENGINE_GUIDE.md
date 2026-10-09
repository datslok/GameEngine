# Engine guide

How the engine works and why it is built this way. This is for people learning the engine, so it explains concepts as well as code. For the exact API, read the headers; for what comes next, see [ROADMAP.md](../ROADMAP.md).

This guide describes the engine as it is now. It is updated at the end of each roadmap phase and after any large feature.

**Covers:** phases 0 to 3, and phase 4 up to texture filtering (lights including spotlights, emissive materials and the sky moon, specular highlights, smooth normals, normal matrix and depth range, mipmaps).

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

So entities that move continuously keep a `PreviousTransform` (their pose at the start of the last tick), and rendering blends: `drawn = previous + (current - previous) * alpha`. Angles take the shortest way around, so something rotating past 360 degrees does not spin backwards for a frame.

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

Systems run with `world.each<A, B>(function)`. It walks the first type's packed array and skips entities that lack the others, so list the **rarest component first**. Inside `each`, do not add or remove components of the listed types or destroy entities: that would reorder the arrays being walked.

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

- `FirstPersonCameraController`: mouse look plus walking on the ground plane.
- `FreeFlyCameraController`: mouse look plus flying (Space and Ctrl for up and down). In both look controllers, holding Shift doubles the speed (running or fast flying).
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

---

## 8. Drawing a frame on the GPU

The renderer uses **SDL_GPU**, SDL3's portable GPU API, with the Vulkan backend. Shaders are written in GLSL and compiled to SPIR-V by `make`.

### Each frame

```
renderer.beginFrame()                 acquire the next screen image, clear colour and depth
renderer.setCamera(camera)            view-projection and camera position, once per frame
renderer.setLighting(lights)          every light, once per frame
for each visible part:
    renderer.drawMesh(mesh, model, material)
renderer.endFrame()                   submit and present
```

The engine collects what to draw from the `World` (`ModelRenderer` + `Transform`, interpolated by `alpha`) and the lights (`collectLighting`).

### Uniforms and why their layout is strict

Data that is the same for every vertex or pixel of a draw, such as matrices, material and lights, goes to shaders as **uniforms**. In SDL_GPU you *push* bytes into a numbered slot, and the shader reads them as a struct. Pushed data stays in effect for the rest of the frame, so lights and the camera are pushed once, and per-object data per draw.

The shader reads those bytes with **std140** layout rules, and C++ does not know them. The classic trap: a `vec3` takes 16 bytes in std140, not 12. To make mismatches impossible, every block uses only 4-component vectors, and each is mirrored by a C++ struct with a `static_assert` on its size (`LightUniformData`, 944 bytes; `MaterialUniformData`, 32 bytes). If anyone changes one side, the build fails.

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
- `PointLight`: shines in every direction from its entity's `Transform` (a torch, a muzzle flash). It has a `range` where it fades to exactly zero.
- `SpotLight`: a point light that shines in a cone, like a flashlight. It stores the direction of its beam and two cone angles.
- `AmbientLight`: a flat fill that stands in for light bounced around the scene. Several add up.

Light colours are 0..1 tints with a separate, unbounded `intensity`, keeping "what tint" apart from "how bright". Material colours are different: they are reflectances, the fraction of light a surface bounces back, so 0..255 fits them.

Each frame, `collectLighting` gathers lights from the `World` (point light positions interpolated like meshes, so a light carried by something moving does not jitter), and `packLighting` turns them into the shader's layout. The shader handles up to **4 directional, 16 point and 4 spotlights**; extra lights are dropped.

### Diffuse: Lambert's cosine law

A matte surface scatters light evenly in all directions, so it looks equally bright from any viewpoint. How much light it *receives* depends on its angle to the light: light hitting at a slant spreads over more area, so each bit of surface gets less. That is Lambert's cosine law:

```
diffuse = lightRadiance * max(0, N · L)
```

where `N` is the surface normal and `L` the direction towards the light (both unit vectors).

### Point lights: inverse square, with a window

Real light from a point falls off as 1/d², because the same energy spreads over a sphere of area 4πd². Two practical changes:

```
falloff = window² / (d² + 1),   window = clamp(1 - (d / range)⁴, 0, 1)
```

- The `+1` keeps it finite when the light is right at the surface (a real bulb is not a mathematical point either).
- Pure 1/d² never reaches zero, so every light would have to be computed for every pixel. The window brings it smoothly to exactly zero at `range`, with zero slope, so there is no visible edge.

### Spotlights: a cone with a soft edge

A spotlight is a point light whose brightness also depends on the angle between its beam's axis and the direction to the surface:

```
cone = smoothstep(cos(outerAngle), cos(innerAngle), dot(-L, beamDirection))
```

Inside the inner angle it is at full brightness (the bright core), beyond the outer angle it gives nothing, and in between it fades smoothly (the penumbra). A real flashlight has that soft edge because its bulb is not a perfect point: different parts of the bulb light slightly different cones, and their overlap blurs the boundary. A hard cutoff would look like a stencil.

The comparison uses cosines rather than angles (a bigger cosine means closer to the axis), so the shader needs no `acos` per pixel; the two cosines are computed once on the CPU. The cone multiplies the same distance falloff as a point light. A spotlight also has a **source radius** `r`: its falloff is `intensity / (d² + r²)` instead of `/ (d² + 1)`. Close to a large source (a reflector, a lit disc) light arrives from its whole area rather than one point, so there is no inverse-square spike; far away (d much larger than r) it is ordinary inverse square again. The demo's flashlight uses r = 4.5, so it is gentle up close but still reaches across the scene.

A light that follows the camera, like the demo's flashlight, is placed every frame in `onUpdate`, not in ticks: the camera moves per frame, and updating the light per tick would make the beam trail behind mouse look. When the duck carries it in MOBA mode, it uses the duck's interpolated pose, for the same reason moving lights are interpolated.

### Specular: Blinn-Phong highlights

Shiny surfaces also reflect light like a blurry mirror, producing a highlight that **moves when you move**. Picture the surface as countless tiny mirror facets tilted randomly around the normal. A facet reflects light from the light into your eye only if it is tilted halfway between them, along the **halfway vector** `H = normalize(L + V)`, where `V` points towards the camera. The question becomes how many facets point along `H`:

```
specular = lightRadiance * specularStrength * max(0, N · H)^shininess
```

High `shininess` (128) means most facets line up with `N`: a small, sharp highlight, like polished plastic. Low (8) is broad and dim. The highlight keeps the light's colour; it is not tinted by the surface colour, which is why highlights on red plastic are white. Materials default to slightly shiny (`specularStrength` 0.25, `shininess` 32); matte surfaces like grass set the strength to 0.

### Emissive: surfaces that glow

Everything above is light a surface *reflects*. A glowing surface, like the moon, a lamp or a screen, also *emits* its own, which does not depend on any light reaching it. The material's `emissive` colour is simply added at the end. Give a glowing object a black base colour and it shows only its emitted light, so it looks the same day or night (the demo's moon, its MOBA marker, and in MOBA mode the duck, which glows a warm yellow on top of its lit texture so it is easy to find from far above).

Emissive surfaces do not light their surroundings: the moon's glow is just its own colour, and the actual moonlight comes from a separate `DirectionalLight`. And a glowing object has a hard edge: the soft halo you expect around a bright light comes from a post-processing effect (bloom), which is on the roadmap.

### The whole sum

```
colour = albedo * (ambient + Σ diffuse) + Σ specular + emissive
```

Light adds up linearly, like superposing intensities, so every light simply adds its share. `albedo` is the surface colour: the texture times the material colour. A surface with no usable normal gets ambient and emissive light only.

Totals above 1 clip to white for now. Handling that gracefully (tone mapping) and gamma-correct colour are later topics.

### Objects in the sky

The demo's moon sphere sits in the direction the moonlight comes from, opposite the light's direction. The real moon is so far away that it shows no **parallax**: walk a hundred metres and it is still in the same direction and the same size. A sky object that should behave like that is kept at a fixed offset from the camera every frame, so its direction and size never change; that is the trick skyboxes use.

The demo deliberately does the opposite: its moon is a fixed object 64 units from where the free camera starts, so you can fly to it in F3 mode in about 20 seconds. The price is parallax: as you move, it shifts and grows like any nearby object, and up close its direction no longer matches the moonlight exactly (a directional light has the same direction everywhere).

---

## 10. Testing

Tests are plain `assert`-based functions, one file per area in `tests/`, registered in `tests/test_main.cpp`. They need **no GPU and no window**, which shapes the design: anything worth testing is pulled out into pure functions (`collectLighting`, `packLighting`, `packMaterial`, `normalMatrix`, `toGpuDepthRange`, `generateSmoothNormals`), and the GPU code just moves their results into buffers.

New features are written **tests first**: write the failing test, watch it fail, then write the code. What cannot be unit tested (shaders, how things look) is checked by eye in the running demo.

The **software renderer** (`render/software/`) is the engine's first renderer, drawing pixels on the CPU. It is kept as a tested reference for rasterising, clipping and depth testing. It does flat shading only and still has its own fixed light.
