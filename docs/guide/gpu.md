# Talking to the GPU

How a frame reaches the graphics card: the frame description, buffers, Vulkan, barriers, timing, compute shaders, ray queries, textures, gamma, dithering, depth precision and the normal matrix.

The renderer is the engine's own, written straight against **Vulkan 1.3** (`render/vulkan/`). Shaders are GLSL, compiled to SPIR-V by `make`. The pass order of a whole frame is in `docs/architecture/renderer.md`; this chapter explains the ideas underneath. How many objects are drawn cheaply is the next chapter, `gpu-scene.md`.

## Describe the frame, then hand it over

The engine first writes the whole frame down as plain data, a `FrameDescription`, and gives it to the renderer in one call:

```
frame = buildFrame(world, camera, alpha)   camera copy, every light, one DrawItem per visible model part
renderer.render(frame, settings, ui)       the renderer decides how to draw it
```

`buildFrame` reads the `World` (each `ModelRenderer` with its `Transform`, interpolated by `alpha`, and the lights through `collectLighting`). It holds no GPU types, so tests can check exactly what would be drawn, and games never touch the graphics API.

Why a list instead of "draw this mesh now" calls? Several techniques must see the whole scene before drawing any of it:

- shadows draw the scene once per light view before the camera's view;
- culling needs every object's bounds;
- a ray tracer needs every object up front.

The renderer keeps its own copy of the camera, because only it knows the size of the image the window handed it, and so the aspect ratio. The engine copies that ratio back into the game's camera, so a mouse click turns into a ray that matches the screen. Details: `docs/architecture/game-interface.md`.

## Uniforms and storage buffers: why the layout is strict

Shaders get data two ways:

- **Uniform blocks**: small, read-only data that is the same for a whole draw or pass (the frame's camera, tone mapping, bloom settings). The shader reads the bytes as a struct.
- **Storage buffers**: plain GPU buffers the shader reads (and may write) like arrays, with no practical size limit. The scene itself (every object, material, light and shadow tile) travels this way.

The trap is that the shader reads bytes by its own layout rules, which C++ does not know.

| Rule set | Used for | Surprise |
|---|---|---|
| **std140** | uniform blocks | a `vec3` takes 16 bytes; every array element is padded to 16 bytes, so 64 floats take 1 KB |
| **std430** | storage buffers | packs tightly: a float 4 bytes, a `vec4` 16, as in C++ |

To make mismatches impossible, every GPU record is mirrored by a C++ struct built from 4-component vectors and matrices, with a `static_assert` on its size (`GpuFrameData`, 432 bytes; `ToneMapUniformData`). Change one side alone and the build fails.

Uniform blocks are also capped: the renderer lets a shader see only the first **4 KB** of each block (`maxUniformBlockBytes`). A shader reading past that gets garbage with no error from anyone, so every uniform struct asserts it stays under the cap, and anything bigger, or anything per object, goes into a storage buffer instead.

Matrices are transposed on the way (`writeColumnMajor`), because the engine's `Mat4` is row-major and GLSL stores matrices column by column.

## The Vulkan renderer

SDL still opens the window and reads the keyboard and mouse; the drawing is the engine's own Vulkan code. Talking to Vulkan directly gives access to what portable layers hide (hardware ray tracing, GPU timers, bindless textures), at a price: a portable layer guesses a sensible answer to a hundred questions, and Vulkan asks every one of them.

Three vendored libraries (`external/`) take the tedious parts:

- **volk** fetches Vulkan's functions from the driver at start-up, so nothing links against a Vulkan library.
- **vk-bootstrap** performs the start-up ritual: create the instance, pick the GPU (a dedicated one when there is one), check its features, make the device, its queue and the swapchain (the ring of screen images).
- **VMA**, the Vulkan Memory Allocator, finds memory for each buffer and image. A GPU has several kinds: fast memory only it sees, memory the CPU writes into for uploads, memory the CPU reads quickly for downloads. VMA picks the kind and packs many objects into each big allocation.

Two Vulkan 1.3 features keep the code short: **dynamic rendering** (a pass names its target images on the spot, instead of pre-built "render pass" objects) and **synchronization2** (a clearer way of writing barriers, below).

### Two workers and their signals

The CPU and GPU run at the same time. The CPU does not draw: it **records** a list of commands (begin this pass, bind that pipeline, draw this mesh) into a command buffer, **submits** it to the GPU's queue, and moves on. The GPU works through the list later, at its own pace.

Two kinds of signal keep them in step:

- a **fence** lets the CPU wait until the GPU has finished a submission;
- a **semaphore** makes one piece of GPU work wait for another (drawing waits until the screen image is free; presenting waits until drawing is done).

How far the CPU may run ahead is the number of **frames in flight** (`EngineSettings::framesInFlight`, switchable live from F5):

| Frames in flight | What happens | Trade |
|---|---|---|
| 1 | CPU records frame N + 1 only after the GPU finishes frame N | a frame costs CPU plus GPU time; least input delay |
| 2 (default) | CPU records N + 1 while the GPU draws N | a frame costs only the slower of the two; up to one frame more delay |

Think of two cooks: one preps the next dish while the other plates this one. The rule that makes it safe: anything the CPU writes for a frame (its command buffer, uniforms, scene buffers, debug line vertices, timestamp pool) has one copy per frame in flight, and that frame's fence says when its copies are free again. Images only the GPU writes need one copy, since the GPU works through submissions in order and barriers order one frame's use after the last's.

### Uniforms on top of buffers

Vulkan has no "push these bytes into slot 3". It has buffers, and descriptors pointing a shader's slot at part of a buffer. The renderer builds the convenient model on top (`FrameUniforms`): each frame gets a 4 MB upload buffer, every push copies its block to the next free spot, and when a draw is recorded each slot's descriptor is aimed at its latest block by a **dynamic offset**, so no descriptor is made per draw. It is a pad of paper forms: never erase a sheet the GPU may still be reading, write on the next one and say which sheet to look at.

### Pipelines and the loading screen

A **pipeline** is a compiled shader program plus every fixed choice around it (blending, depth test, vertex layout). Compiling hundreds of them takes seconds, so `PipelineCompiler` builds them on a thread per core while the game sets itself up, and a loading screen counts them. It draws its text by clearing small rectangles, since no pipeline may be ready yet to draw anything else.

### The validation layers

Vulkan does not check what you ask of it, for speed: a mistake is undefined behaviour, which may work on one driver and corrupt the picture on another. The **validation layers** are the referee: they check every call against the specification and report breaches. Debug builds always load them, and they must report nothing; the release build skips them (they slow every call) unless "GPU validation" is switched on. See `docs/architecture/build.md`.

### Choices kept on purpose

The renderer replaced an earlier portable layer while drawing every reference image bit for bit the same, so it keeps that layer's habits, each commented where set: the viewport is flipped so y runs down the window, counter-clockwise triangles face front, and depth is clamped rather than clipped.

## Barriers: telling the GPU what waits for what

A GPU is not a calculator finishing one command before the next. It is a long production line of **stages** (fetch vertices, run vertex shaders, rasterise, run fragment shaders, test depth, write colours), with many draws and passes moving through at once, overlapping. It also has **caches**: a pass writing an image may leave the newest values near the stage that wrote them, not yet in memory where the next stage looks.

So "draw the shadow atlas, then read it in the main pass" is *not* an order the GPU keeps by itself. A **barrier** restores it, and says three things:

1. **Execution, who waits for whom.** Work in these later stages may not start until work in those earlier stages has finished. For a physicist: it imposes a causal order on two events that would otherwise be spacelike-separated, where neither is guaranteed to come first.
2. **Memory, make writes visible.** Flush these kinds of writes out of their caches and make them visible to those kinds of reads. Waiting alone is not enough; the reader must also see the data.
3. **Layout, how an image is arranged.** A GPU stores an image differently for different uses (a depth target may stay compressed; a sampled or copied or presented image wants another arrangement). Moving from "undefined" tells the GPU the old contents may be thrown away, which is cheaper than keeping them.

Too few barriers and the picture flickers or tears differently on each GPU; too many and the GPU idles, waiting for work it did not need to wait for.

**How the engine writes them.** Raw Vulkan barriers are dense structures of stage and access flags, easy to get subtly wrong. The engine instead names the handful of ways it uses an image (`ImageState` in `render/vulkan/image_state`: `ColourTarget`, `DepthTarget`, `FragmentRead`, `ComputeStorage`, `TransferSource`, `Present` and a few more), and one tested function turns "from state A to state B" into the barrier:

- wait for A's stages;
- flush A's writes, and only writes (two reads in a row need no barrier, so the same read-only state twice records nothing);
- make the data visible to every access of B;
- change to B's layout.

Each image remembers its state, and the state carries across frames, so a pass writing an image this frame still waits for last frame's reading of it. That *write after read* hazard is the easiest one to forget. Drawing code only ever says what it is about to do with an image, and the barrier follows.

Two examples from every frame:

- **The shadow atlas** goes from being read (last frame's main pass) to being a depth target *with its contents kept*, because the shadow cache keeps unchanged tiles from earlier frames; afterwards it goes back to being read.
- **The scene image** becomes a colour target *with its contents discarded*, since the main pass clears it anyway.

The screen image needs one more piece of care. The swapchain hands it over with a semaphore, not a barrier, and the frame's submission waits on that semaphore at the colour-writing stage. The first barrier on the image therefore starts from that same stage, chaining it after the semaphore, so the layout change cannot happen before the image is truly free. At the far end, presenting needs no stage: the semaphore presentation waits on already covers everything before it.

Buffers have no layouts, so their barriers (`bufferBarrier`) are stages and kinds of access only. Full list of states: `docs/architecture/renderer.md`.

## Timing the GPU: timestamps

A stopwatch on the CPU cannot time the GPU's work. Recording a frame takes the CPU well under a millisecond; the GPU draws later, on its own. A clock around `render()` measures only the writing of instructions, plus any wait.

Vulkan's answer is a **timestamp query**: a command saying "when you get here, write down your own clock". The GPU writes its tick counter into a small **query pool**, and the CPU reads the numbers later. Two things turn ticks into time:

- the device's **tick length** (`timestampPeriod`, nanoseconds per tick, often exactly 1);
- how many of the counter's bits are real (`timestampValidBits`). Past its top value the counter wraps to zero like an odometer, so differences are taken modulo 2 to that power. With 36 bits at 1 ns, one turn lasts about a minute.

The renderer writes a stamp before its first pass and one after each, so consecutive stamps bound every pass and the passes add up to the whole frame. A pass that did nothing this frame still gets both stamps at the same moment and lasts zero. Each stamp is written at the "all commands" stage, once everything recorded before it has finished; since the GPU overlaps neighbouring work, a pass's number means "how much later the frame finished because of this pass", which is the number worth optimising.

The stamps can only be read once the GPU has finished, and the frame's fence already says when, so each frame reads the stamps its resources held last time round, with no extra wait. Single-frame times jitter by 10 to 20 percent, so they are averaged over half a second in the F4 readout, grouped into setup, lighting and picture. The CPU's own share of rendering is timed by stage beside them. Details: `docs/architecture/gpu-timing.md`.

**Debug labels** (the `VK_EXT_debug_utils` extension) name stretches of the command list ("SHADOW", "BLOOM") and name images ("Scene colour", "Shadow atlas"). Frame debuggers such as RenderDoc and Nsight then show named passes instead of thousands of anonymous draws. With no such tool attached the driver ignores them, so they stay on in the release build, the one worth profiling.

## Compute shaders: the GPU as a parallel calculator

A **graphics pipeline** takes vertices, rasterises triangles into pixels and colours each pixel. A **compute shader** drops all of that: "run this function N times; here are the buffers to read and write". It is the GPU as it really is underneath, thousands of small calculators in parallel. Clustered shading, ambient occlusion, the fog's grid, the anti-aliasing passes and GPU cloth all run this way.

| Word | Meaning |
|---|---|
| **invocation** | one run of the shader, like one particle in a simulation; it knows its number (`gl_GlobalInvocationID`) and uses it to pick its piece of the work |
| **workgroup** | invocations launched together on one GPU core, a size fixed in the shader (`local_size_x = 32` or 64; NVIDIA runs 32 in lockstep, AMD 64) |
| **dispatch** | a request for a number of whole groups: item count over group size, rounded up (`workgroupCount`); spare invocations past the end return at once |
| **shared memory** | a small, fast memory on the chip that one workgroup sees, for sharing work within the group |
| **push constants** | a few bytes written into the command list itself, the cheapest way to pass a handful of numbers |

Storage buffers carry the data, since they can be large and *writable*, which is how a computation hands back results. A `ComputePipeline` (`render/vulkan/vulkan_compute`) is the compiled shader plus the layout of what it reads and writes; with no fixed-function stages it is much smaller than a graphics pipeline.

Getting results back to the CPU needs the same care as images between passes: the output may still sit in the GPU's caches. So the dispatch is followed by a barrier ("compute writes, then host reads"), the CPU waits on the fence, and on memory the CPU does not keep coherent it invalidates its own view before reading. The other direction is free: whatever the CPU wrote before submitting is visible to the GPU, because submitting guarantees it.

The shader maths that compute and graphics passes share lives in libraries of pure functions (`assets/shaders/*.glsl`), each mirrored in C++ and tested against it on the GPU (`docs/architecture/testing.md`).

## Ray tracing: asking what a ray hits

Rasterising answers "which pixels does this triangle cover?". Lighting effects often need the opposite: "starting here, going that way, what do I meet first?", asked from points all over the level. Testing every ray against every triangle would cost rays times triangles.

Modern GPUs answer in hardware with a **ray query**: a shader hands over a ray and gets back the distance to the first hit, which object and triangle, where on the triangle (two **barycentric coordinates**, the weights blending its corners) and whether it struck the **front** or **back**.

The GPU searches a tree of nested boxes, an **acceleration structure** (a bounding volume hierarchy): miss a box and you miss everything inside, so most of the scene is skipped after a handful of box tests. It has two levels, like a library catalogue:

- **Bottom level**, one per mesh, around its own triangles: built once at upload, then **compacted** (copied into a buffer exactly the size the finished tree needs, often about half).
- **Top level**, one per frame: a list of instances, each a mesh's tree placed by a model matrix. Rebuilt from scratch every frame something traces rays, so moving objects need no bookkeeping.

A ray walks the top level to find instances it passes near, then each one's own tree, carried into the mesh's coordinates by the inverse of its matrix.

**Front and back.** A triangle's front is the side from which its corners go round anticlockwise, which for a closed mesh is the outside. A ray hitting a back face started inside something. The GPU decides this in the mesh's own coordinates, so a mirrored object keeps its outside as its front.

**Masks.** Each instance carries a few bits, and each ray says which bits it sees:

| Mask | Who |
|---|---|
| solid | shadow casters: block light, seen by light-gathering rays |
| visual only | glowing stand-ins for lights (bulbs, the sun and moon discs), already counted as direct light, so gathering rays skip them to avoid counting it twice |
| static | the still level, which light probes move off |
| glass | seen only by the rays that tint light through panes |

Reflections and the debug view ask for every bit.

**Finding the surface hit.** A hit says "instance 12, triangle 300, weights (0.2, 0.5)". Every mesh's vertex and index buffers have a **device address**, their place in GPU memory, which a shader follows like a C++ pointer. A table lists each mesh's two addresses; the shader reads the triangle's three corners and blends their normals and texture coordinates by the weights.

**Checking it.** A debug view (F5, Passes) fires one ray per pixel from the camera and shows each hit's normal, colour or side; it must look exactly like the normal picture. The GPU tests trace 2,000 rays in hardware and compare them with the engine's own CPU raycast over the same triangles: every hit and side agrees. Distances agree to millionths, except for rays grazing a surface, where a tiny shift of the surface moves the hit far along the ray (by the shift over the cosine of the angle), so the allowance grows with one over that cosine.

Ray tracing is optional and **off by default**, under one master switch over every ray-traced feature; on a GPU without ray queries none of it is made. See `docs/architecture/ray-tracing.md`.

## Texture filtering and mipmaps

A texture is sampled once per screen pixel, which is a sampling problem in the signal-processing sense:

- **Up close**, one texel covers many pixels. Taking the nearest texel shows blocks, so the sampler uses **linear filtering**, blending the four nearest texels by distance.
- **Far away**, one pixel covers many texels. Taking one of them samples below the Nyquist limit: fine detail folds back as shimmer and moiré that changes as the camera moves.

The cure for aliasing is a low-pass filter before sampling. A **mipmap** does it in advance: alongside the texture the GPU keeps copies at half, quarter size and so on down to 1 x 1, each texel the average of a 2 x 2 block above (a third more memory in all). When drawing, the GPU estimates how many texels fall in a pixel and reads the level where that is about one. The name is Latin, *multum in parvo*, "much in a small space".

- **Trilinear filtering** blends the two nearest levels, so no line shows where one hands over to the next.
- **Anisotropic filtering** (up to 16 x) serves surfaces seen at a grazing angle, like ground stretching away: a pixel covers a long thin strip of texture, and several samples along it keep it sharp where one level would blur.

Levels are made on the GPU at upload, each blitted from the one above; `mipLevelCount(width, height)` gives the chain's length. Cut-out textures (leaves, fences) are the exception: their levels are built on the CPU so the cut-out keeps its coverage when averaged (`coveragePreservingMips`), or distant leaves would thin away.

## Gamma: storing light the way eyes see it

Eyes respond to light roughly logarithmically, like ears to sound: twice the light does not look twice as bright. So image files do not store amounts of light. They store **sRGB** values, roughly light to the power 1/2.2, spending more of the 256 levels on dark tones where we notice small steps. A stored 128 is only about 22% of white's light.

Lighting maths (adding lights, scaling by angle, averaging texels for mipmaps) is only right on amounts of light, as you add intensities rather than decibels. So:

- colour textures use an `_SRGB` format: the GPU converts each texel to linear light as it reads it, and mipmaps average in linear light; data textures (normals, roughness) are marked linear (`TextureEncoding`);
- material colours are sRGB bytes too (what a colour picker gives), converted by `packMaterial` with `srgbByteToLinear` (`core/srgb`);
- light colours, intensities and emissive colours are linear already;
- shading happens in linear light into a floating-point image, and tone mapping writes the screen, which on an SDR display is an sRGB image (the GPU encodes as it writes) and on an HDR display a linear float one.

A consequence when tuning: linear numbers for dim light look tiny. An ambient of 0.01 shows as about a tenth of full brightness on screen, because the sRGB curve lifts dark values.

## Dithering: noise against banding

Even with gamma, an SDR screen has 256 levels per channel. A slow, dark fade, such as a flashlight's pool at night, changes by less than a level over many pixels, so whole patches round to one level and then jump together: rings, or **banding**. It is quantisation, like measuring a gentle slope with a ruler marked only in whole centimetres.

**Dithering** adds a little noise before rounding, up to half a level either way and different for each pixel. Near a boundary some pixels round up and some down, in proportion to where the true value lies, and the eye averages them back to the in-between brightness (halftone printing, or dither in audio mastering, work the same way).

The noise is sized in the screen's own steps, so the shader encodes to sRGB, adds it and decodes again. It comes from a formula on the pixel's position, **interleaved gradient noise**, which spreads values evenly over every small patch, so the grain is fine and even. A value exactly on a level never moves, so flat colours stay clean, and on average nothing gets brighter or darker; `core/dither` tests both.

## Depth precision: reversed-Z

`Mat4::perspective` follows the OpenGL convention: clip depth from -1 at the near plane to +1 at the far plane. Vulkan wants 0 to 1. `toGpuDepthRange` converts inside the renderer, so nothing outside `render/` knows, and it also turns the range round: near is stored as 1 and far as 0. This is **reversed-Z**.

Why turn it round? A perspective projection stores something like 1/distance, not distance (the same divide that makes far things small). So depth changes fast near the camera and hardly at all far away: two surfaces 1 cm apart at 1 km differ by about one part in a billion.

A float, meanwhile, is a mantissa times a power of two: as many values between 0.25 and 0.5 as between 0.5 and 1, so its values crowd towards 0, like marks on a logarithmic ruler.

- With far at 1, distant depths land just below 1, where floats are coarsest (about 6e-8 apart): the two surfaces get the same depth and flicker through each other, **z-fighting**.
- With far at 0, they land near 1e-4, where floats are a thousand times finer.

The 1/distance squeeze and the float's crowding nearly cancel, like reading an inverse-square signal on a log scale, so precision comes out nearly even from near to far: hundreds of distinct floats between those two surfaces instead of none.

Turning the range round turns every comparison round: nearer now means *larger*. Depth tests keep the greater value, depth images are cleared to 0 (nothing in the way), and a shadow lookup counts a point lit when its depth is at least the stored one. The far plane stays finite, since culling and shadow fitting use it as the draw distance. See `docs/architecture/depth.md`.

## Camera-relative rendering

A float keeps about seven significant digits. Near the origin that is plenty, but 12 km away neighbouring floats are about a millimetre apart.

The obvious way puts the camera's position in the view matrix and each object's position in its model matrix; multiplying them then subtracts two numbers near 12,345, each already rounded to a millimetre, whose difference is a few metres. The leftover rounding shows as vertices jittering by a fraction of a pixel as the camera moves.

So the renderer subtracts first. Everything the main shaders see is measured from the camera: the vertex shader takes the camera's position off each model matrix's translation before any product (exactly what `relativeTo` does on the CPU; a test checks they agree bit for bit), the view matrix keeps only the camera's turn, and lights and shadow lookups shift the same way. The axes do not change, so normals and light directions are untouched. Subtracting two nearby floats is exact up to the small rounding of the small result, so screen positions come out many times closer to the exact answer.

The shadow maps themselves are still drawn in world space, because the shadow cache recognises a tile by its matrices, and camera-relative ones would change whenever the camera moved. The world's positions are still floats; very large worlds would move the subtraction to doubles.

## The normal matrix

Positions transform by the model matrix `M`. Normals cannot: stretch an object along x and a 45 degree slope gets shallower, so its normal must tip towards vertical, but `M` would tip it the other way.

The physics view: a normal is not an arrow like a position; it describes an **area element** (like the cross product of two tangent vectors), and areas transform differently from lengths. The correct matrix is the **inverse transpose** of `M`'s 3 x 3 part. For pure rotations it equals `M`, which is why the difference only shows under non-uniform scale.

`normalMatrix(model)` (`math/mat4`) computes it once per object on the CPU, not per vertex, as the **cofactor matrix times the sign of the determinant**. Normals are normalised afterwards, so the determinant's size does not matter, but its sign does: a mirrored object has a negative determinant, and without the sign its normals would point inwards and it would be lit from behind. A matrix that squashes space flat gets a zero normal matrix.
