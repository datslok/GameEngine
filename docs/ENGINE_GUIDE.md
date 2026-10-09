# Engine guide

How the engine works and why it is built this way. This is for people learning the engine, so it explains concepts as well as code. For the exact API, read the headers; for what comes next, see [ROADMAP.md](../ROADMAP.md).

This guide describes the engine as it is now. It is updated at the end of each roadmap phase and after any large feature.

**Covers:** phases 0 to 3, and phase 4 up to shadows (lights including spotlights, emissive materials and the moon, specular highlights, smooth normals, normal matrix and depth range, mipmaps, gamma-correct colour and dithering, the frame description, shadow mapping, debug drawing, frustum culling, light priority).

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

- `FirstPersonCameraController`: mouse look plus walking on the ground plane, and a jump (Space). The jump is projectile motion: launched at `sqrt(2 g h)` to peak at height `h`, then updated with the exact solution for constant gravity, `y += v t - g t^2 / 2`, rather than small Euler steps, so the arc is the same parabola whatever the frame rate. It lands back at the eye height it started from; with no collision yet, that height stands in for the ground.
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

The shader reads those bytes with **std140** layout rules, and C++ does not know them. The classic trap: a `vec3` takes 16 bytes in std140, not 12. To make mismatches impossible, every block uses only 4-component vectors, and each is mirrored by a C++ struct with a `static_assert` on its size (`LightUniformData`, 2736 bytes; `MaterialUniformData`, 48 bytes; `ShadowUniformData`, 320 bytes).

Uniform blocks are also small by design: SDL lets a shader read only the first 4 KB of each one, and anything past that silently reads as zero. (The engine learned this the hard way: when the shadow tile data grew past 4 KB, whole cube faces of shadow suddenly read as solid black.) Bigger data goes in a **storage buffer**, a plain GPU buffer the shader reads like an array, with no such limit; the per-tile shadow data (each tile's matrix and place in the atlas, about 10 KB for 108 tiles) lives in one, uploaded once per frame. Every uniform struct has a `static_assert` that it stays under 4 KB. If anyone changes one side, the build fails.

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

### Specular: Blinn-Phong highlights

Shiny surfaces also reflect light like a blurry mirror, producing a highlight that **moves when you move**. Picture the surface as countless tiny mirror facets tilted randomly around the normal. A facet reflects light from the light into your eye only if it is tilted halfway between them, along the **halfway vector** `H = normalize(L + V)`, where `V` points towards the camera. The question becomes how many facets point along `H`:

```
specular = lightRadiance * specularStrength * max(0, N · H)^shininess
```

High `shininess` (128) means most facets line up with `N`: a small, sharp highlight, like polished plastic. Low (8) is broad and dim. The highlight keeps the light's colour; it is not tinted by the surface colour, which is why highlights on red plastic are white. Materials default to slightly shiny (`specularStrength` 0.25, `shininess` 32); matte surfaces like grass set the strength to 0.

### Emissive: surfaces that glow

Everything above is light a surface *reflects*. A glowing surface, like the moon, a lamp or a screen, also *emits* its own, which does not depend on any light reaching it. The material's `emissive` colour is simply added at the end. Give a glowing object a black base colour and it shows only its emitted light, so it looks the same day or night (the demo's moon and its MOBA marker).

Emissive surfaces do not light their surroundings: the moon's glow is just its own colour, and the actual moonlight comes from a separate `DirectionalLight`. And a glowing object has a hard edge: the soft halo you expect around a bright light comes from a post-processing effect (bloom), which is on the roadmap.

### The whole sum

```
colour = albedo * (ambient + Σ diffuse) + Σ specular + emissive
```

Light adds up linearly, like superposing intensities, so every light simply adds its share. `albedo` is the surface colour: the texture times the material colour. A surface with no usable normal gets ambient and emissive light only.

Totals above 1 clip to white for now. Handling that gracefully (tone mapping) is a later topic. The whole sum happens in linear light (see gamma, above).

### Shadows: a depth photo from the light

A point is in shadow when something sits between it and the light. Testing that directly for every pixel against every triangle is ray tracing. **Shadow mapping** gets the answer from one extra drawing of the scene instead:

1. Put a camera at the light and draw the scene keeping only depth. The result, the **shadow map**, records for every direction how far the nearest surface is from the light. It is like a photo taken with a rangefinder.
2. While drawing the camera's view, project each pixel's point into that photo and compare: if the photo saw something nearer to the light in that direction, something is in the way, so this light does not reach the point.

Each kind of light needs a different camera:

- A **spotlight** gets one perspective camera looking down its beam, just wide enough for its cone.
- A **point light** shines every way, so one photo is not enough: it takes six, one per face of a cube around the light, each a square 90 degree perspective camera looking along +X, -X, +Y, -Y, +Z or -Z. A direction belongs to the face of its largest component (`pointShadowFace`).
- A **directional light** (the sun) has parallel rays, so its camera has no perspective: it is a box (an **orthographic** projection), like photographing with light that never spreads. The sun lights the whole world, but a shadow map has finite resolution, so the box only covers the 30 units in front of the camera, and stretches 50 units back towards the sun so tall things outside the view still cast into it. As the camera moves, the box would slide by fractions of a texel and every shadow edge would crawl and shimmer; snapping its centre to whole texels (measured across the light's direction) makes it jump a texel at a time, so edges stay put.

All these views share one depth texture, an **atlas** of 8192 x 4096 texels (128 MB), because this SDL version cannot draw into one layer of a texture array. Each frame `planShadows` cuts it into square tiles: one per directional light, one per spotlight and six per point light, for up to 4 directional, 8 spot and 16 point lights. Lights beyond that still give light, just without shadows.

Tiles come in three sizes, 256, 512 and 1024 pixels, chosen by how far the light reaches (`shadowTileSizeFor`): enough texels that one covers about 0.04 units across the far end of the view. A lamp with a range of 5 has cube faces 10 units across at their far end, so 256 texels do; a flashlight beam reaching 50 units gets 1024. A 256 tile holds a sixteenth of the texels of a 1024 one, so small lights are cheap, which is what makes 16 shadowed point lights affordable. If the lights still ask for more than the atlas holds, the least important ones' tiles are halved until everything fits (directional boxes keep full size). The tiles are then packed like a **quadtree**: the atlas starts as 32 squares of 1024, a tile takes the smallest free square that fits, and a square that is too big is split into four quarters, as often as needed. Because every size is a power of two and the biggest tiles are placed first, the quarters of a split square always get used before another is split, so the atlas never ends up with unusable gaps. The shader is told each tile's rectangle in the atlas. The shadow pass draws the scene once per tile, all in one render pass, moving the viewport to each tile. Perspective views are made slightly wider than needed, so the samples around a point near a tile's edge stay on that tile. Models with `castsShadows = false` are left out of the shadow pass (the moon sphere, which surrounds its own light, would otherwise block everything), and the duck holds its flashlight out in front of it for the same reason.

The cost grows with every shadowed light: each tile is one more drawing of every shadow-casting mesh, so a shadowed point light costs up to six. That is why real games shadow only the lights that matter most, and why the cap exists.

Not every cube face is needed, though. A face can only shadow a pixel you can see if its view pyramid overlaps the camera's, and a lamp near you typically has one face pointing back past you at nothing on screen. `planShadows` tests each face with `Frustum::mayIntersect` and gives a face that fails no tile and no draw. The test is the same "all on the wrong side of one wall" idea as culling a sphere: a view is a convex shape, the space between its eight corners, so if all eight corners of one view lie outside one side of the other, the two cannot meet. It is checked both ways round, since a small view next to a corner of a big one is only caught by its own sides. Like the sphere test it can occasionally keep a face that just misses, but never drops one that is needed. A skipped face keeps its place in the list of tiles (with an empty rectangle), so the shader still finds face `f` of a light at its first tile plus `f`, and it counts anything there as lit. The F4 view shows how many tiles were drawn and skipped.
Three practical problems, and their fixes:

- **Shadow acne.** Each texel of the shadow map stores one depth for a whole patch of surface. Tested against itself, a sloped surface comes out half in front of and half behind its own stored depth, in stripes. The fix is a small **bias**: the lookup nudges each point off its surface along its normal by 2.5 texels of its tile (more for points far from the light, where texels cover more ground), so it tests a point just above the surface. GPUs can also bias the shadow pass itself, pushing depths away from the light more on steep slopes, and the engine used to. But that slope-scaled push has no upper limit: a pillar's side seen almost edge-on from a lamp above it got pushed back behind the ground, and its shadow came loose from its base (**peter-panning**, after the boy whose shadow came off). Measured in texels, the normal offset stays in proportion with every tile size.
- **Jagged edges.** A shadow map has a finite resolution, so a plain in/out test draws staircase edges. The sampler does the comparison itself and blends the results of the four nearest texels, and the shader averages nine such lookups in a 3x3 grid (**percentage-closer filtering**, PCF): at an edge, the fraction of samples that are lit becomes a smooth gradient.
- **Resolution.** Tiles are 256 to 1024 pixels across, sized by the light's reach. How much ground one texel covers depends on the view: the moon's box is 30 units wide on a 1024 tile, so about 0.03 units (a cube is 2 units across), sharp. A point light far from the scene is much worse: when the demo's moon was briefly a point light 64 units away, its 90 degree cube faces spread one texel over about 0.13 units, and most faces only saw empty sky. Faces outside the camera's view are now given back (above), but fitting point light views to where the scene actually is remains a possible improvement.

Real shadows from a large source are soft, because near an edge only part of the source is hidden (the **penumbra**, like the edge of the shadow in a solar eclipse), and the further the shadow falls from its caster, the softer it gets. The light's `sourceRadius` will drive that later (percentage-closer soft shadows).

### More lights than seats

The shader has a fixed number of **seats** (array entries) for each kind of light, and the shadow atlas has room for fewer still: 64 point lights can light the scene, but only 16 of them can cast shadows. A level with a torch every few metres has more lights than that, so each frame `prioritizeLights` (`render/gpu/light_priority`) decides who gets a seat and who gets a shadow. Picking the first lights in the list would be arbitrary, and worse, unstable: removing a component moves the last one in its storage into the hole (section 4), so switching the flashlight off could hand a shadow to some other light across the map.

The rules:

1. **Out of view, out of the running.** A point light or spotlight cannot light anything beyond its `range`. If that whole sphere is outside the camera's frustum, it cannot change a single visible pixel, so it takes no seat. This is the same sphere test frustum culling uses for meshes.
2. **Ranked by brightness at the focus.** Each remaining light scores how brightly it lights one point, the **focus**: `intensity × (brightest colour channel) / (d² + r²)`, the inverse-square falloff the shader uses. Physically it is the irradiance the light delivers there. The top 64 point lights and top 8 spotlights get seats; directional lights have no position and are ranked by brightness alone. The shader's range window is left out of the score on purpose: it is exactly zero beyond the range, so every light out of reach of the focus would score zero and tie, and ties fall back to the arbitrary order. Without it, far lights still rank by how far they are.
3. **Shadows to the top casters.** Of the seated point lights with `castsShadows`, the 4 most important get shadows; the others still light the scene, unshadowed. (Every seated spotlight and directional light fits in the atlas.)
4. **Hysteresis.** Walk between two similar torches and their scores cross. With a plain "top 16", the shadow would flick from one to the other, and back and forth every frame while you stand near the midpoint. So a light that had a seat or a shadow last frame counts 1.25 times as important (`incumbentAdvantage`). A rival must be clearly brighter to take it, about 11% closer for an equal light. It is how a thermostat that switches on at 19° and off at 21° avoids chattering around 20°, or a Schmitt trigger in electronics. To know who had what, each collected light carries its `Entity`, and the renderer keeps last frame's choices in a `LightHistory`.
5. **Fading.** Hysteresis stops flicker, but a handover still happens in one frame: a pool of light or a pillar's shadow simply vanishes. So handovers fade instead, over a quarter of a second. The light losing its seat (or its shadow) keeps it while it fades out, and only when it reaches zero does the winner get it and fade in. That way the limits always hold: there are never more than 64 lit or 16 shadowed point lights, even mid-handover. Two cases are deliberately not faded. A light that was not on screen last frame (just switched on, or just come into view) starts at full strength, because nothing showed it before, so nothing can pop; pressing F switches the flashlight on at once. And a light whose range leaves the view is dropped at once, because it was lighting nothing you could see. The strengths reach the shader cheaply: a light's fade is multiplied into its colour on the CPU, and a shadow's strength blends between shadowed and fully lit.
6. **Manual priority.** Brightness is a good guess, but only the game knows that the boss's aura or the lamp on the objective must never go dark. Point lights and spotlights have an integer `priority` (0 by default): a higher priority always wins a seat and a shadow, and brightness and hysteresis only decide between lights of equal priority. It does not bring back a light that is out of view, since that one cannot change a pixel anyway.

**Where is the focus?** For a first-person view the camera is down among the lights, so its position works (the default, `Game::getLightFocus`). A MOBA camera floats about 16 units from the ground it shows, further than a torch reaches: measured there, every torch on screen would score almost nothing, and a dim lamp near the camera would beat them all. So the demo's MOBA mode passes the ground point in the middle of the screen (`MobaCameraController::getLookPoint`). It is like judging street lamps by how they light the street, not the satellite photographing it. The game supplies the point because only it knows where the action is; it reaches the renderer as `FrameDescription::lightFocus`.

To watch it work, press **L** in the demo: it adds 42 coloured lamps, each with a stone pillar beside it. Every bulb glows (that is its emissive material, not its light). All the lamps in view fit in the 64 seats, so they all light the ground, but only the 16 shadowed ones make their pillar cast a shadow. Move around and the shadows follow you; the F4 readout counts the lamps in view, lit and shadowed.

### Objects in the sky

The real moon is so far away that it shows no **parallax**: walk a hundred metres and it is still in the same direction and the same size. A sky object that should behave like that is kept at a fixed offset from the camera every frame, so its direction and size never change; that is the trick skyboxes use. Its light would be a `DirectionalLight`, with the same direction everywhere.

The demo deliberately does the opposite for the sphere: it is a fixed object 64 units from where the free camera starts, so you can fly to it in F3 mode in about 20 seconds. As you move, it shifts and grows like any nearby object, while the moonlight, a `DirectionalLight`, keeps the same direction everywhere, so up close the sphere is no longer exactly where the light seems to come from. (For a while the moonlight was a `PointLight` inside the sphere, which made the light really come from it, but a lamp 64 units away dims across the scene, needs a huge intensity, and gets blurry shadows.)

---

## 10. Testing

Tests are plain `assert`-based functions, one file per area in `tests/`, registered in `tests/test_main.cpp`. They need **no GPU and no window**, which shapes the design: anything worth testing is pulled out into pure functions (`collectLighting`, `packLighting`, `packMaterial`, `normalMatrix`, `toGpuDepthRange`, `generateSmoothNormals`), and the GPU code just moves their results into buffers.

New features are written **tests first**: write the failing test, watch it fail, then write the code. What cannot be unit tested (shaders, how things look) is checked by eye in the running demo.

The **software renderer** (`render/software/`) is the engine's first renderer, drawing pixels on the CPU. It is kept as a tested reference for rasterising, clipping and depth testing. It does flat shading only and still has its own fixed light.
