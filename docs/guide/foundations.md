# Foundations

The ground floor of the engine: how the code is layered, how time and input flow through a frame, how objects are stored, how files become meshes and textures, and how a camera and its rotations work.

## 1. The big picture

The engine has to carry very different games: a shooter, a top-down MOBA, and later a dress on a mannequin, a solar system and a flying hero toppling buildings. So its core knows nothing about any of them. There is no "player", no weapon, no ability in the engine itself; that code lives in `gameplay/` and in each game's folder under `games/`.

### Layers

The source in `src/` is stacked in layers, and **a folder may include only from layers below it**. Roughly, from the top:

```
app/                 picks a game, starts the engine
games/               one folder per game
engine/              the main loop, the Game interface, settings, commands
render/  platform/   the GPU renderer / the window, SDL input, command socket
cloth/  gameplay/    cloth solvers / movement, camera controllers
assets/              loading files
scene/               meshes, materials, lights, camera, frame description
physics/             shapes, raycasts, the level, character controller
core/  input/        small shared tools / the input snapshot
math/  ecs/          vectors, matrices, quaternions / entities and components
```

The exact table, with what each folder must never include, is in [LAYOUT.md](../LAYOUT.md), and a test checks every include against it. Each rule pays for itself:

- **Games never include `render/`, `platform/` or SDL.** A game's whole simulation can run with no window and no GPU. The tests run the demo for many ticks this way, and a multiplayer server will need exactly the same thing.
- **The renderer receives plain data.** Each frame the engine hands it a description (meshes by handle, matrices, materials, lights) and nothing else, so the renderer can change completely behind that seam without game code noticing.
- **`input/` knows no SDL.** Game code asks "is W held?" with the engine's own `Key`.

At start-up `main` makes a game and an `Application`, the engine. The engine owns the window, the renderer, the input snapshot, the `World` of entities and the `AssetManager`, and calls the game through a handful of hooks (`engine/game.h`; details in [game-interface.md](../architecture/game-interface.md)).

### Several games on one engine

Four games share the engine today:

| Name | What it is |
|---|---|
| `lightlab` (the default) | A sunlit test ground for shadows and bounce light, with a real sky, a street of lamps and a two-room building |
| `demo` | A night scene of lamps, floodlights and an obstacle course |
| `clothlab` | Stations for testing cloth, including a dress on a mannequin |
| `forest` | A jungle valley showcase kilometres across |

`game.exe --game <name>` picks one. Each extra game is also a check on the layering: if the engine truly knew nothing about the demo, a new game should need no engine changes, and so far none has.

**A registry, not an `if` chain.** `app/game_registry.cpp` holds one table of `{name, function that makes the game}`. The command line, the golden image runner and the tests all read it, so adding a game is a single line. The table stores *recipes* rather than games because the golden images want a fresh game for every view, so nothing switched on for one view leaks into the next. Each recipe is a plain function pointer that captures nothing, which lets the whole table be a compile-time constant.

**Sharing controls by composition.** The games want the same F1 first-person, F2 MOBA and F3 free-camera modes, with the same body and tuning. The obvious design is a base class both inherit from, but inheritance makes every child carry every choice of the parent: a dressmaking game wanting an orbit camera and no body would fight machinery it never asked for, and any change in the parent silently changes every child (the "fragile base class" problem). Instead the controls are an object, `PlayerControls` (`gameplay/player_controls.h`), that a game *holds as a member* and forwards to. A game wanting something else holds something else. Most engines build gameplay this way: parts a game assembles, not a family tree of classes.

**One settings file, a section each.** All games share `settings.cfg`. Each game's values sit under a section named from its title (`demo.grenade.throwSpeed`, `lightLab.sun.solarHours`). When one game saves, the other games' lines are read back from the file and written out unchanged, so no game can wipe another's. How settings are declared once and drive the file, the F5 debug window and text commands is in [settings-and-commands.md](../architecture/settings-and-commands.md).

## 2. The frame loop and time

### Two clocks

A game does two different jobs over time:

1. **Simulate**: move characters, turn spinners, resolve collisions.
2. **Render**: draw a picture as often as the frame cap allows (`EngineSettings::targetFPS`, 120 by default).

If the simulation took one step per rendered frame with whatever time had passed, it would behave differently on fast and slow machines. The physics analogy is direct: integrating `x += v * dt` with a varying, sometimes large `dt` gives different trajectories, and with collisions a big step can carry an object straight through a wall ("tunnelling"). Integrators are most predictable with a **fixed step**, so the simulation advances in **ticks of exactly 1/120 s**, and rendering runs on its own clock.

### The accumulator

`FixedTimestep` (`core/fixed_timestep.h`) keeps a bank of time not yet simulated:

```
each frame:
    accumulator += real time since last frame   (clamped to maxFrameSeconds, 0.25 s)
    while accumulator >= tick and the budget allows:
        run one tick
        accumulator -= tick
    alpha = accumulator / tick                   (0..1)
```

A slow frame runs several ticks to catch up; a fast one may run none. The clamp handles long stalls (dragging the window, a breakpoint): without it the engine would try to simulate seconds of backlog at once, making the next frame slow too.

The clamp is not enough when a single tick costs more real time than the 8.3 ms it simulates (a huge cloth, say). Then each frame owes more ticks than the last, like a pump that cannot keep up with the inflow, until the game crawls at a few frames a second. So ticks also have a **budget**, `maxTickSecondsPerFrame` (1/60 s of real time a frame by default): the first tick always runs, another only if one more of the average cost still fits, and the rest are dropped rather than owed. An overloaded simulation then runs in slow motion at a smooth frame rate, and the camera, which moves per frame, stays responsive. Cheap ticks still catch up after a hitch, since a dozen ticks of 0.3 ms fit easily. A cloth ticked on the GPU costs the CPU nothing, so it is charged its measured GPU time instead. Details: [engine-core.md](../architecture/engine-core.md).

The clock decides only *how many* ticks a frame runs; what each tick computes never looks at it. That keeps the simulation **deterministic**: the same commands give a bit-identical state on the same build, which a test checks by hashing the world.

### Interpolation: why motion looks smooth

After the ticks, a fraction `alpha` of a tick is left over. Drawing objects exactly where the last tick left them would stutter, because frames and ticks do not line up: some frames would show the same pose twice, others would skip one.

So entities that move continuously keep a `PreviousTransform` (their pose before the last tick), and drawing blends between the two:

  drawn = previous + (current − previous) × alpha.

Rotations blend along the shortest turn between the two orientations (slerp, see [Orientation: quaternions](#orientation-quaternions)), so something spinning past 360 degrees never spins backwards for a frame. Things that teleport, such as the move-to marker, deliberately have no `PreviousTransform`: blending would make them slide across the map. Spinners and fireflies go one better and compute their pose from the total simulation time, so no rounding error builds up over hours.

### Input becomes commands

Each frame runs in this order:

1. `onInput(world, input)`, once: clicks and key presses become **commands** ("move to this point", "throw a grenade").
2. `onFixedUpdate(world, tickSeconds, simulationSeconds)`, zero or more times. It has **no `Input` parameter, on purpose**.
3. `onUpdate(world, input, frameSeconds, alpha)`, once: the camera moves here, so mouse look is as smooth as the frame rate rather than stuck at 120 Hz.
4. Render with `alpha`.

Why keep input out of ticks? A click is an *event* that belongs to one frame. A frame running three ticks would see it three times if ticks read input directly, and a frame running none would miss it. Turning input into commands first makes each click act exactly once. It is also the shape networking needs: clients send commands, the server simulates.

## 3. Input

`Input` (`input/input.h`) is a **snapshot of one frame**, built from events so a key pressed and released between two frames is still seen. It holds no SDL types:

- **Keys**: `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased`, using the engine's `Key`, which names physical positions on a US layout, so WASD sits under the same fingers on any keyboard.
- **Mouse buttons** work like keys, and each press also records its **exact position and time**, so a quick click lands where it happened, not where the cursor ended the frame.
- **The rest**: mouse motion summed over the frame (for mouse look), the cursor position, the window size, focus, and whether the mouse is captured or confined to the window.

`platform/sdl_input.cpp` is the only place that turns SDL events into this. The window reports raw facts only; rules such as "Escape releases the mouse" belong to game code.

## 4. Entities and components

### The idea

Instead of a class tree (`Player : Character : GameObject`), the engine uses **entities, components and systems**:

- An **entity** is only an ID: "thing number 7".
- **Components** are plain data structs attached to entities: `Transform`, `ModelRenderer`, `PointLight`, `CharacterMovement`...
- **Systems** are plain functions run over every entity having a given set of components.

The flashlight is an entity with a `Transform` and a `SpotLight`; a spinning cube is `Transform` + `ModelRenderer` + `Spinner`. New kinds of object come from new *combinations*, not new classes.

### Handles that go stale safely

An `Entity` is an **index plus a generation**. When an entity is destroyed its slot may be reused, but the generation counts up. An old handle still carries the old generation, so `world.isAlive(oldHandle)` says false rather than quietly pointing at whatever took the slot. `Entity{}` is never alive and means "none".

### Sparse sets

Each component type has its own `ComponentStorage<T>` (`ecs/component_storage.h`), a **sparse set** of three arrays:

| Array | Holds |
|---|---|
| `components` | every `T`, packed with no gaps, so systems walk memory in order (what the CPU cache likes best) |
| `entities` | which entity owns each packed element |
| `sparse` | for each entity index, where its component sits in the packed array |

Adding, removing and lookup are all constant time. Removing moves the last element into the hole, which keeps the array packed.

Systems run as `world.each<A, B>(function)`. It walks the first type's packed array and skips entities lacking the others, so **list the rarest component first**: walking 3 lights and checking each for a `Transform` beats walking 10,000 transforms looking for lights.

Because removing reshuffles an array, and adding may reallocate it, neither may touch an array while `each` is walking it. The `World` counts the walks in progress (they can nest):

- During a walk, `destroy` and `remove` go into a queue and are carried out in order when the outermost walk ends. It is like a physics step that computes every force before moving anything: no update sees a half-updated world, and a system may destroy what it walks without a second loop.
- Adding a component of a type being walked throws instead, since a deferred add could not hand the new component back; `addLater` queues it explicitly.

## 5. Assets and handles

The `AssetManager` (`assets/asset_manager.h`) owns everything loaded from files or made in code: meshes, images and glTF models.

- **Each file is loaded once.** Asking for the same file again returns the same handle. The cache key includes the options that change the result: a mesh's smoothing settings, a texture's vertical flip and encoding.
- **Handles, not pointers.** A `MeshHandle` or `TextureHandle` is a typed index; the type tag only stops a mesh index being passed as a texture. The default handle means "none": a material with no texture is plain colour.
- **Append-only.** Nothing is ever unloaded, so handles never go stale and need no generation, unlike entities. "What is new since last frame" is simply "every handle past the count the renderer has", and the engine uploads those before each frame, so a game may load at any moment.
- **Colour or data.** Each texture carries an encoding: sRGB for pictures meant to be seen, linear for numbers stored in pixels (normal maps, roughness). Reading numbers as sRGB would bend them.

The renderer numbers its GPU copies exactly as the handles are numbered, so finding a mesh's GPU buffers is one array access. Big generated scenery (the forest's rocks and hangings) can be kept only in the render format, saving most of the memory. The full list of calls is in [assets.md](../architecture/assets.md).

## 6. Meshes and normals

### Two formats

- **`Mesh`** (`scene/mesh.h`) is the *import* format: positions plus triangles, each corner optionally carrying a normal and a UV (a texture coordinate). Loaders, physics and the software reference renderer use it.
- **`IndexedMesh`** (`scene/indexed_mesh.h`) is the *render* format: a list of unique vertices (position, normal, UV and tangent, laid out exactly as the GPU reads them) and 32-bit indices into it. Corners identical in every part are merged, so a shared vertex is stored and shaded once.

How meshes made in code (sheets, surfaces of revolution) and meshes that deform every tick (cloth) work is in [meshes.md](../architecture/meshes.md).

### What a normal is

A **normal** is the direction a surface faces, and every lighting term depends on it. A file may give one for each corner. If not, the corner gets its triangle's **face normal** (the cross product of two edges, normalised), and the model looks faceted: one flat shade per triangle.

### Smooth normals, on request

A curved model without normals, like the teapot, looks blocky. `generateSmoothNormals(mesh, creaseAngle)` (`scene/smooth_normals.h`) estimates the smooth surface the triangles approximate:

- For each corner, average the normals of the faces sharing its **position**. Position, not vertex index, because files often duplicate vertices along seams (the teapot has 319 such duplicates).
- Weight each face by its **angle at that corner**, so the answer depends on the shape and not on how it was cut into triangles. A fan of ten thin slivers on one side should not outvote one big triangle on the other.
- Count only faces within the **crease angle** (60 degrees by default) of the corner's own face. Gentle curves are averaged; true edges, like a cube's or the teapot's rim, stay sharp.

It is **opt-in** per load (`MeshLoadOptions{.smoothNormals = true}`), because only the game knows whether a shape is meant to be faceted: a stealth fighter's panels meet at shallow angles but must stay flat. Normals given by the file are never replaced.

### Normal maps: bumps without geometry

Between vertices the normal is only interpolated, so a surface with few triangles looks like smooth plastic. A **normal map** is a texture whose texels are small tilts of the normal. Light then behaves as if the surface had grooves, weave or scratches while the geometry stays flat: silhouettes do not change and nothing casts a shadow on itself. In physics terms it perturbs the boundary condition, not the boundary.

**A direction stored as a colour.** A texel holds three numbers c between 0 and 1, read as the direction n = 2c − 1. An untouched surface is (0, 0, 1), stored as (0.5, 0.5, 1): the pale blue every normal map shows. The texture is read as linear numbers, never decoded as sRGB, which would turn 0.5 into about 0.21 and tilt every flat texel. glTF's `scale` multiplies the sideways parts, so 0 flattens the bumps and 2 exaggerates them.

**The frame the tilt is written in.** The direction is not a world direction. It is measured in a frame glued to the surface: the **tangent** T (the way the texture's u grows), the **bitangent** B (the way up the picture) and the normal N. The world normal is

  n_x T + n_y B + n_z N,

a change of basis, like writing a vector in a sphere's local unit vectors instead of x, y, z. Because the frame travels with the surface, one brick picture works on a wall, a floor or a barrel. T and B are the partial derivatives of position with respect to u and v, the columns of the UV map's Jacobian; on one triangle they follow from two edges and their UV differences by inverting a 2×2 system.

**Handedness.** B is nearly redundant, being ±(N × T). So each vertex stores T plus a sign w (`MeshVertex::tangent`, a `Vec4`), and the shader rebuilds B = w (N × T). The sign is −1 where an artist mirrored part of the picture to paint both halves of a face at once. Corners on such a seam share position, normal and UV but not the sign, so they stay separate vertices.

A mirrored *object* (a model matrix with negative determinant) flips the rebuilt bitangent as well, since

  (Ma) × (Mb) = det(M) M⁻ᵀ(a × b),

so the vertex shader multiplies w by the instance's handedness, and its bumps stay bumps.

**Why MikkTSpace.** A vertex needs one frame, but each of its triangles has its own, and averaging them involves choices: weights, where to split, whether triangle order matters. Bakers used to choose differently, and a normal map is only correct in the exact frame it was baked in (like reading a vector's components in the wrong basis), so mismatches showed as seams and odd shading. Morten Mikkelsen's MikkTSpace fixed one order-independent answer that Blender, Substance, Unity and Unreal now share, and glTF asks for it when a file has no tangents. The engine uses a file's own tangents when present and otherwise runs the vendored MikkTSpace (`scene/tangents.cpp`).

- MikkTSpace's bitangent follows increasing v, while glTF counts v down from the top of the image, so glTF UVs are handed over as (u, 1 − v) (`Mesh::uvOrigin`).
- A test regenerates the tangents Blender exported for a Khronos test model and gets the same ones, signs included.
- MikkTSpace expects the shader to use the interpolated normal and tangent as they arrive, unnormalised, and to normalise only the result, because its baker does exactly that.

**Odds and ends.**

- A material without a normal map gets scale 0 and the white stand-in texture, which yields exactly the plain normal, so the shader needs no branch.
- Shadow lookups keep the geometric normal: tilts far smaller than a shadow texel would only add noise there.
- Far away, one mip texel averages many tilted normals into a shorter vector, a sign of bumps too fine to see. The engine turns that shortening into extra roughness (Toksvig's correction), so distant bumpy metal goes soft instead of sparkling. See [shading.md](../architecture/shading.md).
- The demo shows Khronos test models beside its path: CompareNormal (one shape without and with a map) and grids of bumps whose pictures are turned and mirrored cell by cell. Every cell should look lit from the same side; a wrong sign or a flipped green channel shows as cells lit from below.

## 7. Cameras

### A lens, and separate controllers

`Camera` (`scene/camera.h`) is only a lens: position, forward and up directions, vertical field of view, aspect ratio, and near and far planes. It builds its matrices on request, so zooming by narrowing the field of view needs no extra work. It has no movement logic of its own.

Controllers in `gameplay/camera/` move it:

| Controller | Behaviour |
|---|---|
| `FirstPersonCameraController` | Mouse look only. Walking, jumping and crouching belong to an invisible first-person body, a capsule moved by the character controller, and the game puts the camera at its eyes each frame. Shift sprints at 1.5 times walking speed. |
| `FreeFlyCameraController` | Mouse look plus flying, Space and Ctrl for up and down; Shift flies twice as fast. |
| `MobaCameraController` | A fixed angle following a target, or panning when the cursor touches the edge of a confined window. |

Look controllers keep the direction as **yaw and pitch**, spherical coordinates: yaw 0 looks along −Z, positive yaw turns right, and pitch stops at 89 degrees so the view never flips over the pole. Movement tuning and control modes are in [gameplay.md](../architecture/gameplay.md).

### The matrices

A point travels through a chain of coordinate systems, each step a 4×4 matrix:

```
model space --model--> world space --view--> camera space --projection--> clip space
```

- The **model** matrix places an object (its `Transform`).
- The **view** matrix moves the whole world so the camera sits at the origin looking down −Z.
- The **projection** matrix applies perspective: once the GPU divides by `w`, distant things shrink.

Why 4×4 for 3D? A translation is not linear in three coordinates, but in four (x, y, z, 1) it becomes a matrix product, so a whole chain of moves, turns and the perspective collapses into one matrix. `Mat4` is row-major in memory (`values[row][column]`), multiplies column vectors (`M * v`), and uses right-handed coordinates. How the renderer then maps depth to the GPU (reversed-Z) and draws relative to the camera for precision is in [depth.md](../architecture/depth.md).

### Orientation: quaternions

A `Transform` stores its rotation as a **unit quaternion** (`Quat`, `math/quat.h`), not as three angles. Three Euler angles (turn about x, then y, then z) are easy to read but have two faults:

- **Gimbal lock**: when the middle turn reaches 90 degrees, the first and last axes line up, so two angles do the same thing and one direction of turning is lost. Near that pose a small change of orientation needs a huge change of angles, and a tumbling body passes near it constantly.
- **Blending**: interpolating each angle separately does not follow the true in-between orientation, except for turns about a single axis.

A quaternion is a scalar plus a vector, q = (w, **v**), as a complex number is a real part plus an imaginary one. (Hamilton, who invented them in 1843, coined *scalar* and *vector* for these two parts.) Their product is

  (w₁, **v**₁)(w₂, **v**₂) = (w₁w₂ − **v**₁·**v**₂, w₁**v**₂ + w₂**v**₁ + **v**₁×**v**₂).

The cross product inside is why order matters, as two quarter turns of a book give different results in different orders.

A turn by θ about a unit axis **n** is q = (cos θ/2, **n** sin θ/2), and it rotates a vector by the sandwich **p**′ = q **p** q*, where q* = (w, −**v**) is the conjugate that undoes the turn. In 2D a unit complex number e^{iθ} turns the plane by plain multiplication; in 3D the sandwich keeps the result a pure vector, and each side carries half the angle.

That half angle has a consequence a physicist knows from spin-½: q and −q are the same rotation. Unit quaternions live on a 4D sphere that covers the rotations twice (SU(2) covering SO(3)), and a full 2π turn stores −1.

What the engine does with them:

- **Composing**: `a * b` turns by `b` first, then `a`, the same order as matrices acting on column vectors. `toMatrix` reads the rotation matrix straight from the components, with no trigonometry.
- **Blending** (`slerp`): the shortest arc between two orientations on that sphere, at constant angular speed, so a frame drawn between two ticks shows the true in-between orientation.
  - Because of the double cover, the target is negated when the two lie on opposite sides, which makes the turn take the short way round.
  - Closer than about 1.8 degrees, a plain linear blend renormalised to unit length is used instead. It avoids dividing by the sine of a tiny angle and is just as accurate there, and at 120 Hz it is the common case.
- **Errors as vectors** (`toRotationVector`, the log map): any turn as one vector, axis times angle. `(target * current.conjugate()).toRotationVector()` is what is left to turn, and in which direction, to reach a target; it can drive a spring just as a displacement does, which is how future ragdoll joints will pull towards a pose. `fromRotationVector` (the exp map) goes back, turning an angular velocity times a time step into the turn made in that step.
- **Angles where they are natural**: mouse look keeps a yaw and pitch, a character its own yaw (since "turn at most this fast towards that heading" is easiest in one angle), and a spinner three angles and their speeds. Each builds its quaternion at the end (`fromAxisAngle`, `fromEulerAngles`), and nothing ever reads angles back out of one.
