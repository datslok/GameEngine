# Testing

How the engine checks itself: the CPU unit tests, the determinism check, golden images, the GPU test tier, and the test that keeps the docs honest.

The rule of thumb behind all of it: each layer catches what the one below cannot. Unit tests check the maths; the GPU tier checks that the shaders compute the same maths; golden images check whole pictures; and the eye, in the running game, checks what no number captures. The exact files, tolerances and tables are in [the testing reference](../architecture/testing.md); the steps for writing a test are in `tests/CLAUDE.md`.

## Unit tests

The tests are plain C++: one file per area in `tests/`, each with a `testXxx()` function that calls `assert` on what must hold. No framework, no GPU, no window.

That last constraint shapes the code. Anything worth testing is pulled out of the renderer into **pure functions**, functions whose result depends only on their arguments: `packLighting` (lights into the GPU's records), `packMaterial`, `normalMatrix`, `toGpuDepthRange`, `generateSmoothNormals`. The GPU code then only copies their results into buffers. Games are written the same way, never touching the window or the renderer, so a test can build a whole game and run it headless.

**Tests first.** Each feature starts with a test that fails, then the code that makes it pass. A failing test proves the test can fail; one written after the code may pass for the wrong reason.

`tests/test_main.cpp` registers every test and runs them in three groups:

| Group | Run by | What |
|---|---|---|
| Routine | `make test-release` (or `make test`) | Everything in daily use, with a progress bar; `tests.exe --times` lists the 30 slowest |
| Slow | `make test-slow` | A few cloth solver tests that take many seconds and guard rarely changed code |
| Parked | `make test-parked` | Work on hold (the light probes, the cloth): left out of the routine run until it resumes |

The release build is the routine run: it is much faster, and `-O2` exposes bugs and warnings `-O0` hides. Both builds keep `assert` working (never `-DNDEBUG`, which would turn every check into nothing). Run by an AI, the suite goes through `tools/visible_tests.ps1`, which shows the progress bar in a window.

Shared helpers keep test files short: small physics levels in `tests/test_levels.h`, driving the demo headless in `tests/demo_helpers.h`, one job system for the whole run in `tests/test_jobs.h`.

### Comparing floats

Exact equality is rarely the right test for floating point. Physics tests compare within small named tolerances, and checks of physical laws are phrased as laws: energy is conserved, Fresnel reflectance reaches 1 at grazing angles, a white surface under uniform light returns exactly that light (the **furnace test**, a closed box at uniform temperature: whatever goes in must come out).

## Determinism

The simulation promises **bit-identical** results: run one build twice on the same commands and every tick ends in exactly the same state. That is what will let a client and a server agree, and replays play back.

`engine/state_hash` boils the state down to one number: `hashSimulationState` feeds every component the fixed tick changes (transforms, characters, first-person bodies, particles, grenades), field by field, into **FNV-1a** (`core/hash`), a simple hash where any changed bit changes the result completely. Floats are hashed by their exact bits, so even 0 and -0 differ.

`tests/determinism_test.cpp` plays the demo and the light lab twice side by side with the same scripted input, comparing hashes after every frame. A third run, one grenade throw or one jump short, must *disagree*: otherwise the test might pass only because the hash ignores what happened. A control experiment, in physics terms.

Keeping that promise rules some things out inside a tick: reading the clock, iterating containers ordered by memory address, letting threads finish in whatever order. The job system returns results in a fixed order for this reason, and a new component that ticks change must join the hash.

## Golden images

Golden images are the renderer's reference photographs. Each game lists fixed views (the demo's start, its HDR twin, the normal map gallery, the lamp field, the floodlights, the moon; the light lab's towers, rooms and night street; the cloth lab's stations; the forest's rim, Ring, wall and dusk). Each view is set up in a fresh game with default settings, ignoring `settings.cfg`, and simulated to a fixed moment, so it is the same scene every time.

- `make golden-save` renders each view off screen at 1280x720 and keeps two pictures: the scene before tone mapping, in half floats, and the final image. Half floats are packed losslessly into an ordinary PNG twice as wide.
- `make golden` renders them again and compares pixel by pixel, writing a heatmap of differences to `build/golden/`.

Before capturing, every history is started afresh and the frame is drawn repeatedly until it settles: 32 frames (two jitter cycles) with anti-aliasing on, 960 when light probes are traced. Eye adaptation snaps straight to its target.

On the same GPU, driver and shaders the result is identical to the bit, so the references live per GPU (`tests/golden/README.md`) and any difference at all is a real change. A pass allows at most one step of difference in at most 0.1% of pixels; a whole picture one step brighter fails. Bit-exact pictures are far stronger evidence than "looks the same to me": they are what made replacing the whole graphics API safe.

Golden runs need a GPU and take a while, so they are not part of `make test`, and run only when asked.

## The GPU test tier

Shading maths exists twice: GLSL that the GPU runs, and a C++ copy (a **mirror**) that the unit tests check against physics. Nothing but care ties the two together. A typo in the shader that the mirror lacks would show only as a slightly wrong picture, and a golden image says *that* the picture changed, not *which function* is wrong.

So the shaders' maths lives in libraries of pure functions (`brdf.glsl`, `tone_mapping.glsl`, `lighting.glsl` and dozens more in `assets/shaders/`). They read no textures and use no pixel position or screen derivatives, since a compute shader has none. The render passes `#include` them, and so do small compute shaders in `tests/gpu/shaders/`, which run the very same functions on thousands of fixed inputs: a grid of roughnesses and angles for the BRDF, brightness from black to a thousand times white for tone mapping, every edge and corner of the cube for the shadow face. The results are read back and compared with the mirror on the same inputs.

`make gputest` runs this (its own program, `build/gputests.exe`, with the validation layers on) and then the golden images. A few tests go further and run real passes (the depth measurement, the sky's tables, eye adaptation, ray queries, the light probes, the cloth) on inputs whose answer the CPU works out.

### Why "equal" is the wrong test

The GPU will not produce the CPU's bits, for honest reasons:

- It **fuses** a multiply and an add into one operation with one rounding (FMA): more accurate, but different from the CPU's two roundings.
- Vulkan lets built-in functions be **slightly approximate**: `inversesqrt` may be 2 ULP off, division 2.5, and `pow` is built from `exp2` and `log2`, so its error grows with the exponent. A **ULP** (unit in the last place) is the gap between a float and the next one, about 1.2 × 10⁻⁷ of the value.

So two results agree when they differ by at most

  `absolute + relative × |expected|`

(`core/float_tolerance`). The absolute part matters near zero, where 0 and 10⁻³⁰ differ by "100%"; the relative part matters for large values, where a fixed allowance would be smaller than one float step.

A tight tolerance is safe because of the **gap** between the two kinds of difference. Rounding lands around 10⁻⁷ to 10⁻⁵ relative; a real bug (a wrong constant, swapped arguments, a missing π) is off by percent or more. The report prints, for each function, how much of its tolerance the worst value used, so a shrinking margin shows before anything fails. Planted bugs (Fresnel's fifth power made a fourth, the falloff's fourth power made a cube) fail by thousands of times the tolerance.

### When the problem amplifies rounding

One comparison teaches something general. A near-mirror's highlight is a tall, narrow spike. GGX's distribution is

  α² / (π d²), with d = 1 − (n·h)²(1 − α²),

and near the peak `d` is a small difference of two numbers close to 1. The GPU and the CPU compute the halfway vector `h` their own ways, so `n·h` may differ by one float step δ. That shifts `d` by about 2δ, and the brightness by about 4δ / d relative. For a shiny surface (α = 0.03) seen 10⁻⁴ from the peak, `d` is about 10⁻³, and one float step already moves the result by 2 × 10⁻⁴.

This is the problem's **condition number**: how much a relative error in the input grows in the output. Physicists know it as an experiment run where the answer swings wildly with the setup. The fix is not a looser tolerance everywhere: the specular allowance grows as 1 / d, generous exactly where the maths is touchy and tight elsewhere. Diffuse light has no spike and keeps the plain tolerance.

Noise has a similar story. The noise that turns soft shadow samples keeps only the fractional part of a large product, so at 4K pixel coordinates few digits survive, and results may differ by a few thousandths: invisible as grain, but not rounding-sized. It gets its own allowance (4 × 10⁻³), measured round the circle, since it wraps at 1 and 0.9999 sits next to 0.0001.

What the GPU tier leaves alone is **texture filtering** (bloom's blur, soft shadow sampling, the BRDF table lookup): how hardware blends neighbouring texels is not something a table of numbers checks cleanly, so the golden images keep covering it.

## The docs test

The documentation is tested too. `tests/docs_test.cpp` runs with the routine suite and fails when:

- a source includes a folder above its own layer (the table in `docs/LAYOUT.md`);
- an instruction file, the README, a reference file or a guide chapter outgrows its size budget;
- a backticked path in a checked doc does not exist;
- a doc is missing from `docs/INDEX.md`, or a source or shader is claimed by no reference file's `Covers:` line;
- two checked docs share a run of twelve identical consecutive words.

The last check uses **shingling** (Broder 1997, how search engines spot copied pages): every run of twelve words is hashed, and a run already seen in another doc is a repeat. Each fact then has one home, and the others link to it.

`tests.exe --docs-report` finds what a test cannot judge, and only prints it: dates and timings outside the build log and roadmap (a measurement belongs to its history, while a setting's value is fine where it is), and backticked code names the code no longer has. The end-of-phase docs review reads it.

## The software renderer

`render/software/` is a CPU rasteriser kept as a tested reference for rasterising, clipping and depth testing. It does flat shading only, with its own fixed light, and is used only by tests.
