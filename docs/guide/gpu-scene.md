# Drawing many things

How the renderer draws thousands of objects and lights cheaply: the GPU scene, culling, the depth prepass and clustered shading.

The previous chapter, `gpu.md`, covers how commands, buffers and barriers reach the GPU. This one is about scale: the work that grows with the number of objects, pixels and lights, and how each piece is kept from multiplying.

## The GPU scene: a catalogue instead of envelopes

The naive way to draw hands the GPU an envelope per object: push its matrices and material as uniforms, bind a descriptor set naming its textures, issue one draw call. The shadow pass then does the same for every caster in every shadow tile. A scene with moving lights easily reaches thousands of envelopes a frame. Each costs the CPU a little (recording, state checks, descriptor writes), and the GPU, which works through big batches far faster than many small ones, waits on that bookkeeping.

The **GPU scene** sends a catalogue instead. Once per frame the renderer writes into **storage buffers**:

- every object's **instance**: world matrix, normal matrix, handedness, material number and a few more fields;
- every distinct material, deduplicated;
- every light, carrying its own shadow information.

Every texture lives in one big array set up once. A draw then says only "draw mesh M for entries 40 to 149 of the draw list", and the shaders look up the rest themselves: the vertex shader finds its object from its instance number, the fragment shader the material from the number the object carries, and the textures from the numbers the material carries. **Nothing is bound per object.** Record layouts and bindings: `docs/architecture/gpu-scene.md`.

### Instancing and the draw list

With nothing per object left to bind, two draws differ only in their mesh (and in their drawing style: whether both sides show, whether holes are cut). So the renderer groups the visible objects by those, and each run of one mesh becomes a single **instanced draw**: `vkCmdDrawIndexed(indices, instanceCount, ..., firstInstance)` draws the mesh `instanceCount` times, and in the shader `gl_InstanceIndex` counts up from `firstInstance`.

That number indexes the **draw list**, a buffer of instance numbers holding every view's entries one after another: the camera's visible objects in batch order, then each redrawn shadow tile's casters, then the other views (the velocity pass, the reflection probe's faces, a mirror, glass). The grouping keeps a fixed order, so the same frame always gives the same list, which the determinism and golden image tests rely on.

### One record, many views

Each instance holds the object's world matrix, so the camera and every shadow tile share one record, and each view multiplies in its own matrix on the GPU. The camera's view sits in the frame's uniform block; a shadow tile's is a **push constant**, a few bytes written straight into the command list before the tile's draws. Each view may also pick a coarser version of a mesh by its distance and size on screen (level of detail: `docs/architecture/level-of-detail.md`).

### Bindless textures

A **descriptor** is the GPU's handle to a resource (an address, a size, a format), and shaders normally see them through small sets bound per draw. With **descriptor indexing** a shader can declare an array of unknown size, `sampler2D textures[]`, and pick an element by a number it read from a buffer. The renderer keeps one such array of 16,384 slots: slot 0 is a white texture (a missing texture multiplies by 1), and each uploaded texture takes its asset handle plus one.

Two details matter:

- **`nonuniformEXT`.** A GPU shades pixels in lockstep groups (32 at once on NVIDIA, one instruction for all). Neighbouring pixels can belong to different objects with different textures, so one group may need several textures at once. Unless told otherwise, the hardware may assume the whole group reads one, and sample the wrong texture for some pixels. Marking the index `nonuniformEXT` makes it handle each distinct index. It is the classic bindless bug, so the qualifier is never optional.
- **Filling slots while frames are in flight.** Normally a descriptor set must not change while a submitted frame may use it. The bindless set is flagged so a new texture may fill a slot no running frame reads, and slots never written are fine as long as nothing reads them. Textures are never unloaded, so a written slot never changes.

### Lights in the catalogue

Light records live in storage buffers too, with room for 1024 point lights and 256 spotlights (plus a few directional ones), each carrying its own shadow details (first tile in the atlas, strength while fading, emitter size). With clustered shading (below) a light costs only where it shines, so every light the buffers hold is drawn. Which of them get shadows is a separate, smaller budget: `docs/architecture/lights-and-shadow-atlas.md`.

### Why moving the matrix product changed pixels

Doing the matrix product on the GPU changes the order of arithmetic: `(view × model) × vertex` becomes `view × (model × vertex)`. Mathematically equal, but each multiply rounds, so vertex positions differ in the last bit or so. Usually invisible.

But a shadow test is a **threshold**: is this point's depth beyond what the light saw? At a point lying almost exactly on the threshold, a last-bit change flips one of a soft shadow's samples from lit to blocked, and the pixel changes a little. It is a ball balanced on a ridge falling left or right on a breath of wind: tiny causes, visible effects, but only at the knife edge. The lesson recurs throughout the renderer: **reordering floating-point arithmetic changes bits**, so changes meant to leave the picture alone are checked bit for bit against golden images, and changes that reorder arithmetic are made separately from changes in meaning.

## Frustum culling: skip what cannot be seen

A camera sees a **frustum**: a pyramid with its tip cut off, between the near and far planes and inside the field of view (for the sun's shadow box, just a box). Anything wholly outside is drawn for nothing, so each view skips it.

The frustum is six planes, each with an inside, taken straight from the view-projection matrix: a point is on screen when its clip coordinates satisfy `-w <= x <= w` and so on, and each of those six inequalities is a plane. Every object gets a **bounding sphere** (its mesh's box placed by its model matrix, the radius grown by the largest scale). A sphere is outside if its centre lies more than a radius beyond any plane: six dot products per object, far cheaper than drawing it.

Two rules:

- **Each view culls with its own frustum.** Something behind you can still throw a shadow into view (the low sun behind a tree behind you), so culling shadow casters with the camera's frustum makes shadows vanish whenever their caster leaves the screen, a classic bug.
- **The test may keep, never drop.** Near a frustum's corners it can keep a sphere that is just outside: a little wasted drawing, never a missing object.

**Casters by a light's reach.** A point light has six shadow views. Most objects are nowhere near most lights, so a cheaper test runs first: an object can shadow what the light lights only if its bounding sphere touches the light's sphere of reach,

  `|c - p| < r + range`

(compared squared, so no square root). One test per object and light, shared by all six faces, before the frustum tests on the few survivors. It is nearly exact: a blocker lies between the light and a lit point, and lit points are within range, so a caster wholly beyond it blocks nothing. The one gap is where a soft shadow's filter reads texels beyond the range on a grazing surface, and there the light has faded to almost nothing. The culling functions: `docs/architecture/game-interface.md`.

## The depth prepass: settle who is in front, then pay for light

A pixel can be covered by several surfaces: floor, a pillar in front, a lamp in front of that. Each surface a triangle lays over a pixel is a **fragment**, a candidate colour, and the **depth test** keeps the nearest. The waste is in the order: draw the floor first and the GPU shades its fragment in full (every light, every soft shadow), then the pillar arrives nearer and overwrites it. That thrown-away work is **overdraw**.

GPUs fight it with **early depth testing**: when a fragment shader neither writes depth nor discards, the hardware tests depth *before* shading and drops fragments behind what is stored. That helps only if the near surface happened to come first, and the renderer orders draws by mesh, not distance.

The **depth prepass** makes the order irrelevant:

1. Every opaque object is drawn writing depth only: no colour, and no fragment shader except for cut-outs, which must still punch their holes. That is cheap, and afterwards the depth image holds the nearest surface at every pixel.
2. The main pass draws everything again with the depth test set to **equal** and depth writes off. A fragment survives only if its depth is exactly the stored one, so the expensive shading runs once per pixel, for the surface you actually see.

It is surveying a site before building: one quick pass with a tape measure settles where everything stands, and the costly work happens only where it counts. The prepass's depth also feeds every screen-space pass after it: clustered shading, ambient occlusion, contact shadows, reflections, anti-aliasing.

**The catch: equal means bit for bit.** Both passes must compute each vertex's depth to the last bit, or the fragment fails the test and leaves a black speck. Two consequences:

- The prepass uses the main pass's own vertex shader, not the shadow pass's depth-only one, whose world-space arithmetic rounds differently.
- Even one source can compile differently in two pipelines: a compiler may fuse a multiply and an add into one **FMA** instruction (rounding once instead of twice) in one pipeline and not the other. Declaring `invariant gl_Position;` is GLSL's promise that the same expression on the same inputs gives the same bits wherever it is declared.

**Lights from behind skip their shadows.** A light behind the surface adds nothing (Lambert's cosine is negative), but the shader used to pay for its soft-shadow lookup before finding that out. `facesLight` asks the same question first, on the same normal, so the wasted lookups go and the picture stays identical.

The prepass is a setting (F5, Passes; PREPASS in the F4 timings), so the two can be compared live. It pays most where objects hide each other; where the cost is lights rather than overdraw, the cure is the next section. Pass details: `docs/architecture/renderer.md`.

## Clustered shading: sort the lights into boxes first

Without help, every pixel loops over every light and asks "do you reach me?". In a field of lamps a point on the ground is lit by a handful, and every other answer is "no", at each of millions of pixels. Each "no" is cheap, but their count grows as lights times pixels.

The fix is the idea behind the physics broad phase, or the cell lists of a molecular dynamics code: sort first, then compare only neighbours. The view is cut into a grid of **clusters**, and before shading a compute pass writes down which lights can reach each one. A pixel finds its cluster and asks only those lights.

**The grid.** The screen is cut into 64-pixel square tiles, and each tile along the view direction into 24 **slices**. Slice k starts at depth

  near · (far / near)^(k / 24),

so each slice is the same *factor* deeper than the last, not the same distance. That is a logarithmic scale, as on a slide rule: near the camera a centimetre matters, far away a few metres do not, and perspective shrinks far things in just that proportion, so each cluster looks about as deep on screen as it is wide. A pixel's slice is the formula run backwards, one logarithm per pixel:

  slice = floor(log(depth) · scale + bias).

**Is this light near this box?** The pass puts a box round each cluster's piece of the frustum and asks whether the light's sphere of reach touches it (is the box's nearest point within the radius? the same test physics uses for a sphere against a box). A spotlight lights only a cone, so it is also tested against the cone with a neat trick (Wronski, "Cull that cone"): split the way from the cone's tip to the cluster into its part along the beam and its distance from the beam, rotate that picture by the cone's half-angle, and you have the distance from the cone's slanted side. If that exceeds the cluster's size, the beam misses.

Both tests are **conservative**: they may say "maybe" for a light that adds nothing, never "no" for one that reaches. A "maybe" costs a pixel one distance check; a wrong "no" would leave a dark patch.

**One bit per light.** Each cluster's list is a row of bits, one per light: 1024 for point lights and 256 for spotlights, 160 bytes a cluster. A pixel walks the set bits from the lowest up, and the lowest is light 0, so it meets lights **in slot order**, the same order a plain loop would.

That matters more than it seems. Floating-point addition is not associative: (a + b) + c can differ from a + (b + c) in the last bit. Here a pixel adds exactly the lights that reach it (every one is in its list, and those it skips added exactly zero), in exactly the same order, so the result matches the plain loop to the last bit. The golden images are identical with clustering on and off, a strong test that no cluster ever misses a light.

One subtlety: a pixel near a slice boundary may compute a slightly rounded logarithm and land in the neighbouring slice, whose box the light might just miss. So every box is grown by a thousandth of its depth on each side, more than any rounding, and tests check thousands of random lights and points.

**Inside the compute pass.** One invocation handles one cluster. Rather than each reading every light from memory, the 32 invocations of a workgroup share the work: each fetches one light, turns it into the camera's frame and leaves it in **shared memory** (a team spreading papers on one table). A `barrier()` waits until all 32 are on the table; each invocation tests its cluster against all 32 and writes one 32-bit word of its row; another barrier, then the next 32 lights. Each invocation writes only its own words, so the result never depends on which finishes first.

**Barriers for a buffer.** The bit rows live in a GPU-only buffer: the compute pass writes it, the main pass (and the fog) read it. So the GPU is told the order twice: before the pass, last frame's reads must finish before the rows are overwritten (a *write after read*, where only order matters); after it, the writes must be flushed and made visible to the readers (a *read after write*).

**Pixels in step.** Pixels are shaded in groups of 32 that run in lockstep, like a rowing crew: if one must walk 7 lights and its neighbour 3, the neighbour waits. At a cluster edge, neighbours have different lists. So each word of bits is first OR-ed across the group (`subgroupOr`, one instruction), and all 32 pixels walk the combined list together, reading each light's record at the same moment. A pixel meets a few of its neighbours' lights this way, but those cannot reach it (they would be in its own list otherwise), so they add nothing.

**Seeing it.** The F5 Cluster heatmap (Passes) colours each pixel by how many lights its cluster lists: black for none, then blue, green, yellow and red at 16 or more. It shows that far, deep slices and tiles seen at grazing angles list more lights than reach any one point, since their boxes are loose; each extra listed light costs only a distance check. Details: `docs/architecture/clustered-shading.md`.
