# Build log: Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 3 of 5)

## Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 3 of 5)
### 6d: global illumination (Oct 19 2026 onwards) (continued)

- `ddgi-relocation-surfaces` (Oct 20 2026): the user found the house lighting worse since cascades, looking from the inner doorway at the window: a dark block on the wall below and left of the window and a bright smear down its near edge.
  - **Narrowing it down:** a capture of that view on the build before cascades and on the current one showed it. Switching suspects apart in scratch builds:
    - relocation off removed the smear but left the wall dark and flat
    - a self-shadow bias of 0.6 changed nothing
    - moving the lattice off quarter metres moved the patches
    - 1 m spacing (the old layout) gave the old picture.
  - **The cause**, found by the CPU mirror of relocation run against the lab's own triangles: the densest lattice (x.25 and x.75 m) puts a layer of probes inside the 0.3 m window wall and two columns exactly on the planes of the window's sides, which inside the wall are the seams where its boxes touch (two faces on top of each other).
    - Relocation rays started at distance 0, so from a seam a ray met the neighbouring box's face from outside at once.
    - The probe took itself for one in the open beside a surface, neither moved nor turned off, and lent the wall the boxes' dark insides (or, at the window's edge, the sunlit ground).
  - **The fix:** relocation rays now skip hits nearer than 1 mm (`DDGI_RELOCATION_MIN_DISTANCE`), so such a probe sees the solid's real inside and steps out into the room; probes standing in the open window hole stay.
    - Chosen over another lattice offset, since levels put walls anywhere and any offset lands some probes on some level's faces.
    - A light lab test relocates every densest probe in and round the window wall against the level's triangles (`traceRelocationRay`) and failed before the fix.
  - **Golden images:** `sunlit-room` (up to 38 of 255, on the window wall), `flashlight` and `back-room-lamp` (at most 1 of 255 in the final image) saved anew; every outdoor view and the demo's matched bit for bit.
- `ddgi-densest-cascade` (Oct 20 2026): the user found the bounce light's detail fell off too soon round the camera.
  - The densest cascade lit only 3.6 m round the camera at full weight and 1.6 m up and down, so from eye height the floor at one's feet, the ceiling and a room's far walls already read the 1 m probes.
  - Of three options (taller windows for all, every cascade larger, or only the densest larger) the user chose the densest alone, since detail matters most near the camera: 31 x 17 x 31 probes (6.1 m and 2.6 m), the coarser three still 21 x 13 x 21; Medium a densest of 25 x 15 x 25, Low 19 x 11 x 19.

  Decisions:
  - **The atlas layout:** the atlases' rows were one layer of a cascade, which needs every cascade the same width, and a 31-wide layer of distance squares would be 17,298 texels, past the largest image side.
    - Now every probe is numbered straight through the cascades and the squares are wrapped into a near-square sheet (`probeAtlasGrid(probes)`: 65,535 probes fit in 4,608 texels a side).
    - Each cascade's record carries its own counts (`GpuProbeCascade` 96 to 112 bytes); the push constants carry the sheet's columns and rows.
    - A probe's cascade is found by walking the at most six records (`probeCascadeHolding`), rather than a lookup table to keep in step.
  - **Checked that the rework alone changes nothing:** with the densest set back to 21 x 13 x 21 and the old sheet's shape forced (273 x 84 squares) every golden view matched the old references bit for bit. With the new near-square sheet they differed by a few half-float steps (up to 25 in one view's scene image), the GPU's bilinear filter rounding texture coordinates on a sheet of another size, compounded by 960 frames of bounces.
  - **The GPU cascade tests** now use cascades of different sizes. Their first version showed that a densest window reaching past the next cascade's full weight seeds the planes it brings in partly from the flat ambient when no third cascade covers them; the real layouts always have one, so the test's coarser cascade was widened instead.
  - **Measured** (release, RTX 5080, light lab golden views, against the same build at 21 x 13 x 21): the probe pass 0.22-0.56 ms instead of 0.18-0.49 (+13-25%; 3,346-5,793 probes traced instead of 2,826-5,028), whole frames up to 0.07 ms longer, the engine's video memory 795 MB instead of 700.
  - **Golden images:** every light lab view with probes saved anew (finer bounce light further from the camera); the overview, which no cascade reaches, and the demo's matched.
- `ddgi-relocation-nudge` (Oct 20 2026): the user saw the light lab's back room glow like the outdoors through the front door from 12 m or more away, and dark again up close.
  - **Tracked down step by step:** captures from the door at 6, 10, 15 and 25 m on this build and on the one before showed it was there since cascades. In scratch builds:
    - painting the flat ambient's share magenta showed the light came from probes
    - painting each cascade its own colour showed the room read the 4 m cascade from 15 m
    - capping the distances' variance (a guess at Chebyshev leaking past the eaves) changed nothing
    - relocation off made the room dark.
  - **Why**, from the CPU mirror of relocation against the level: the 4 m cascade has one probe inside the back room, its grid point 0.23 m over the floor, under its too-close threshold (0.1 of 4 m).
    - The 2021 reference's rule moved it towards the farthest front face it saw, which lay out through the front door.
    - Five steps walked it 1.6 m (its limit) to 0.85 m from the doorway, where it saw mostly the sunlit outdoors and lent that to the room's back wall, which faces the door.
  - **The fix:** a probe too close to a surface now steps straight away from it until it stands 0.2 of a spacing off it, which is what the rule is for and cannot herd probes into openings.
    - The same-surface guard (`DDGI_SAME_SURFACE_COSINE`) is gone with it.
    - The nearest surface's direction is that of the relocation ray that met it nearest, a few degrees off its normal, so a probe drifts a few centimetres sideways as it rises.
  - **Tests:** a probe low over a floor beside an open doorway rises rather than walking out, and the back room's 4 m probe stays more than 2 m from the door; both failed before.
  - **Not fixed by it:** from about 25 m the back room lies beyond the coarsest cascade's full weight (29 m) and takes the flat sky ambient.
  - **Golden images:** `sun-disc` (up to 35 of 255: the room behind its door is dark now, where the old reference showed the leak), `towers` and `sunset-hdr` (the coarse cascades' light on the house and ground) saved anew, and the indoor views too (at most 1 of 255); the demo's matched.
- `ddgi-far-cascades` (Oct 20 2026): the user asked whether lighting is dynamic far away. Direct light and sun shadows are (to 200 m), but bounce light only reached the fourth cascade's 29 m; beyond, the flat sky ambient lights everything as open sky.
  - **The change:** the cascades already allowed six, so the defaults now use them: two more 21 x 13 x 21 cascades 8 m and 16 m apart, carrying bounce light to 58 m and 116 m at full weight (Medium five, to 8 m; Low four, to 8 m).
  - **What it does:** they are too coarse to see into a house (an 8 m lattice may have no probe inside one), but light landscapes: in the overview, which no cascade reached before, the towers' shaded sides now take the grass's green.
  - **Measured** (release, RTX 5080, light lab golden views, against four cascades): the probe pass 0.36-0.78 ms instead of 0.22-0.56 (5,261 probes traced instead of 3,346 in the towers view; the far cascades' probes near the ground), whole frames 0.14-0.23 ms longer, the engine's video memory 828 MB instead of 795.
  - **Golden images:** every light lab view saved anew; the demo's matched.

#### Night, the eye and the street

- `night-sky` (Oct 20 2026): the user asked for a moon and a starry sky at night, and chose a real star catalogue and artistic brightness dials (no exposure yet).
  - **The moon's place** comes from Meeus's mean elements and largest terms; checked against 2026's two eclipses (the model aligns both within 0.05 degrees) and the equinox and solstice (0.005 degrees).
  - **Its phase is geometry:** the disc in the sky pass is shaded by Lommel-Seeliger's law (the closed-form disc average checked against a numerical one to four digits), and its light follows Allen's measured phase curve (a quarter moon 9% of full).
  - **Its sky reuses the sun's:** scattering is linear in the light, so the sky view table holds a second sky lit by the moon, stacked under the sun's (an atlas, like the probes', rather than a layered image, so no binding or image class changed); the other two tables are shared.
  - **Directional lights are now dimmed by the air before they are ranked**, so once the sun sets the moon takes the shadow cascades.
  - **The stars** are the Yale Bright Star Catalogue (9,096 stars, trimmed to 285 KB).
    - Coloured by Ballesteros's temperature and Planck's law against the CIE curves.
    - Turned by one rotation from the sidereal angle (the sun's hour angle plus its right ascension).
    - Drawn as tent-weighted splats so a star's light stays whole as it crosses pixel borders; the air dims them, the moon hides them.
  - **Found on the first golden run:** the boosted stars (thousands of times real) showed through the day sky, so the light lab fades them in from the sun 4 to 14 degrees below the horizon.
  - **Moonlight:** the first night view was nearly black at the planned moonlight of 0.05; the default is 0.25 (the full moon above the air; the sun is 4).
  - **The clock** now carries past midnight into the day and the year (`advanceLabClock`), so the moon moves on through a running night.
  - **Not measured on their own:** the moon's sky doubles the small sky view table (192 x 108 texels, 30 steps each), and the stars are 9,096 instances of six vertices.
  - **Golden images:** a new `moonlit` view (day 88, 21:00, facing the waxing gibbous moon); every other light lab view saved anew (the day's young crescent moon adds a faint sky and light, at most 2 of 255 in the final images); the demo's, which has no atmosphere, matched.
- `eye-adaptation` (Oct 20 2026): trying the night sky, the user found the pillars and ground looked lit only by the sky, not by the moon.
  - **Checked:** the game handed the renderer the right moon (56 degrees up, south-south-east, 0.09 after the air), and with the moon as bright as the sun the night drew exactly as a day under a high southern sun, so its light, direction and shadows worked.
  - **The cause:** at 1/35 of the sun's light every surface sat in the tone curve's toe, where lit and shaded sides squeeze together and lose their colour.
  - **Choice:** of three fixes (eye adaptation, a night exposure dial in the lab, a brighter moon), the user chose eye adaptation.
  - **How it works,** all on the GPU, read by tone mapping the same frame:
    - a histogram of the scene's luminance in stops (256 bins, shared-memory atomics per workgroup)
    - the mean of its middle (50th to 90th percentile: the black sky and the moon and bulbs left out)
    - a target that makes up a share of the difference from a reference
    - an exponential ease, slower into the dark (3 s) than into the light (0.7 s).
  - **Tuned** by the golden views' measured means (daylight -1.2 to -1.9 EV, the sunlit room -6.1, the moonlit night -6.6, the demo's night -5.3 to -7.4, its moon view -16):
    - the reference 0.3 keeps the daylight towers as they were
    - strength 0.75 made the moonlit night a dull day, 0.5 keeps it a night
    - the highest exposure +4 EV (not 6) keeps a black-sky frame from being boosted 64 times.
  - **Found on the way:** the main pass left the scene image readable by fragment shaders only, and a barrier from one read state to another carries no memory visibility, so it now leaves it readable by compute shaders too.
  - **GPU tests** match the histogram pixel for pixel and the exposure to 1e-6.
  - **Golden images:** every view saved anew, both games (the demo's night brighter, the sunlit room's interior brighter, the daylight views within a few steps).
- `light-lab-street` (Oct 20 2026): a test ground for many lights at night in the lab's real setting (sky, moon, cascades, eye adaptation), where the demo's lamp field has none of them.
  - **The user's choices:**
    - Point lights with a solid shade over each bulb (so every lamp has soft shadows and its own cube map shapes the pool, with no cone to tune), over spot lights (one tile each, but only 8 may cast shadows and a 140 degree beam stretches one tile badly) or no shadows.
    - A 300 m street with lamps every 10 m (all within the probes' 116 m when standing in the middle), over 750 m at a real 25 m.
    - The automatic switch at dusk plus K to flip it until the sun next asks for a change.
  - **The windows** are glowing static panes with no light component: the probes' rays bring their glow back, so they light the street through global illumination alone.
    - First night view: the windows at a radiance of 1 were 30 times the lamp-lit road and clipped white under the exposure the road sets; dimmed to about 15 times, they keep some colour.
  - **The moonlit view**, facing the moon in the south, now looked at the north row's backs, so its camera moved south of the street (z = 80).
  - **Measured** (debug exe, RTX 5080, 2560x1440, the street-night view, 100 captures):

    | Street | Main pass | Probe pass |
    |---|---|---|
    | dark | 0.91 ms | 0.62 ms |
    | lamps lit, casting no shadows | 1.11 ms | 0.80 ms |
    | lamps lit with their shadows | 2.71 ms | 0.86 ms |

    - So the 60 lamps' soft shadows (32 samples per lamp reaching a pixel, two to four do) cost 1.6 ms and their shading 0.2.
    - The probe pass grows with the hit lights' checks and shadow rays.
    - The roadmap's TAA is what lets soft shadows take far fewer samples.
  - **Golden images:** a new `street-night` view; every other light lab view saved anew (the probes and the sun's fitted cascades see the street, the trees on its ground are gone, the exposure follows the ground; the sun-disc sky identical value for value); the demo's matched.
  - **A delay into the dark:** trying it, the user found that switching a room's light off looked like a slow fade: the exposure began rising at once (an exponential makes its biggest step first), brightening the room while the light died away. A real eye is the other way round, nearly blind at first.
    - Into the dark it now holds for 1 s (`darkAdaptationDelaySeconds`), counting only the time past the delay so frame length does not matter.
    - A brighter frame resets the wait, so a flickering light never sets it off.
  - **Bigger discs:** the user also asked for the sun's and moon's discs at five times their real size by default (for looks; shadows keep the real size).
  - **The user's look:** the user then tuned the look by eye in the debug UI, and their settings became the defaults:
    - the exposure bias 1 (was 1.5)
    - dark adaptation 1 s (was 3)
    - the highest exposure +1 EV (was +4: dark scenes brightened by at most a stop)
    - the moon's full intensity 0.05 (was 0.25).
  - **A glow beside the doorway:** the user saw the house's outside wall glow beside the lit doorway at night, reaching further on Low than on Ultra. A camera readout on F4 (`cameraText`) gave their exact view. Experiments from it:
    - bloom off left the glow
    - bloom and probes off removed it
    - ignoring probes behind the wall left it
    - no lamp light at probe rays' hits removed it
    - no further bounces left it
    - lamp light only at hits outside the house removed it.

    So it is probe parallax, not a leak through the wall: the probes just in front of the wall see the lamp-lit jambs and room at grazing angles, which the wall itself cannot. Left for now (far less visible with the +1 EV ceiling); per-pixel short-range lighting added to the roadmap beside reflections.

#### Probes in the demo, and Lumen

- `demo-probes` (Oct 20 2026): the light probes in the demo, which until now drew with the flat night ambient only.
  - **What was added:**
    - One `ProbeCascades` round the camera.
    - The obstacle course marked static like the ground (it was in the static level already but not marked, so probes could have sat inside its blocks).
    - Each lamp field and floodlight switch one scene event (`probeEventAround` gained a form taking several points, one box round them all, so a hundred lamps restart one region, not a hundred).
  - **Fast lamps:** the user chose the flying lamps as fast lights over still ones: their bounce follows them instead of trailing for the 2 s memory.
  - **Measured:** estimated beforehand as roughly doubling the probe work while they are on, since the field covers the ground. Measured (debug exe, RTX 5080, lamp-field golden view, 120 captures) far less: the probe pass 1.65 ms instead of 1.38 (+20%), the frame 4.29 instead of 4.03; the other views' probe pass 0.28-0.75 ms.
  - **The look:**
    - Out of the lights the night is darker, since the flat ambient (0.012) came equally from every side and the probes' rays that hit the ground or the course bring back almost nothing (ambient occlusion, for free).
    - The floodlit stone and grass bounce onto the course's shaded faces and the teapot.
    - No probe buried in a moving object showed as a dark patch in any view.
  - **Golden images:** every demo view saved anew; the moon's had changed only within tolerance (54 pixels by one half-float step), since it looks at the sky.
  - **Seen while checking, not looked into yet:**
    - The light lab's `street-night` once differed from its reference in 5 pixels (up to 29 half-float steps) and the next run matched bit for bit.
    - The demo's first saved `lamp-field` was the odd one of four runs (22 pixels, up to 2 steps).
    - Both views have many lights at probe hits, so something there is not quite reproducible from run to run. `main` before this branch does the same (`street-night` passed, then failed by the same 73 pixels): the view lands on one of two pictures, not random noise.
    - Where to look first: many lights at probe rays' hits (`keepBrightestLight` ties, the order lights reach a hit).
- `lumen-research` (Oct 20 2026): the user asked how Lumen is made and tuned, and how to build something like it.
  - **Read:** Epic's SIGGRAPH 2021 and 2022 Advances talks in full (with speaker notes), their documentation and release notes, a copy of the scalability settings, and the published alternatives (GI-1.0, Brixelizer, kajiya, ReSTIR GI, radiance cascades, hash-grid caches, denoisers); written up as `docs/research/LUMEN.md`.
  - **What it showed:**
    - Our DDGI cascades are Lumen's outer layer (a world-space cache round the camera).
    - What we lack is its inner one, probes on the visible pixels, which Epic built after trying an irradiance field like ours and finding that irradiance computed at the probe, not the pixel, leaks and looks flat (the doorway glow found in the light lab).
    - With hardware ray queries we need neither its surface cache (our probe rays already shade their hits from the GPU scene) nor its mesh distance fields (deprecated by Epic in 5.6).
  - **The user chose:**
    - the software tracer next, as a global distance field instead of the voxel grid the roadmap named (Epic dropped voxels as leaky and slow, and AMD's Brixelizer builds the same field at run time)
    - TAA first in 6e
    - the Lumen-like layer far down the line, in phase 14, where 6e's "short-range lighting per pixel" item is folded into it, since the notes showed they are one feature.

#### The moon's phases and night vision

- `moon-phase-contrast` (Oct 22 2026): the user found the light lab's exposure right in dark rooms and under the full moon, but a moon that is not full too dark. Raising the exposure bias fixed that, but made the room with the window too bright once the lamp next door was on.
  - **The cause:** the moon is dimmed by the real phase curve (Allen's), which leaves a quarter moon at 9% of full (3.5 stops) and a fat crescent at 2.6% (5.3 stops).
    - The eye adaptation makes up only half a difference, and brightens by at most a stop, which the full moon already uses. So all of a lesser moon's shortfall showed.
    - The bias multiplies every scene, the lit room's too.
  - **Choices weighed:**
    - a dial for the lab's moon alone
    - adaptation that gets stronger in the dark (engine-wide, but it would change the dark rooms the user was happy with, and nights drift towards day)
    - or a higher exposure limit (it brightens the full moon and dark rooms too, and leaves the gap).

    The user chose the moon's dial.
  - **`LightLabNightSettings::moonPhaseContrast`:** the phase curve raised to this power, as the eye's sense of brightness, roughly a power of the light (Stevens), squeezes the stops.
    - First 0.5 (a quarter moon 1.75 stops below full); the user tried it and chose 0.25 (0.9 below).
    - The full moon is unchanged. With the contrast at 1 every golden view matched the old references bit for bit.
  - **A sky too bright:** then the user found the moon lit the sky too much against the ground, so a night looked almost like a sunrise.
    - Checked: the moon's sky is the sun's physics at the moon's light, phase included, so a moonlit sky is a daytime sky 400,000 times dimmer, orange glow round a low moon and all, as long exposures show.
    - The eye sees it otherwise: below about 3 cd/m2 the colour-blind rods take over from the cones, so the night looks grey and cool, blue relatively brighter and red darker (the Purkinje shift).
    - And with the exposure at its highest, the ground sits in the tone curve's toe.
  - **Choices weighed:**
    - night vision in tone mapping (engine-wide, the physical cause, but the dark rooms change too and the thresholds are dials until the engine has physical units)
    - or a dial for how much the moon lights the air (small, not physical, and the warm glow stays).

    The user chose night vision.
  - **Night vision:** each pixel blends towards the rods' grey, Ward Larson's scotopic luminance (1997) divided by white's, a little blue (the blue shift, 0.5), by the rods' share 1 - smoothstep(rods, cones, max(the pixel's stops, the eye's)).
    - The `max` makes a pixel go grey only when both it and the scene are dim, so lamps in a dark street and a dark corner of a sunlit room keep their colour.
    - The eye's state is the scene's mean eased like the exposure (the same hold into the dark), kept in two of `GpuEyeAdaptation`'s spare floats, so colour drains a moment after a light goes out, not at once.
  - **The thresholds by measurement:** a small decoder read the golden scene images' means in stops.
    - The moonlit field is near -8.2 EV but the sunlit room by day near -6.2: the lab's moon is about 6,700 times too bright against its sun (1/60 instead of 1/400,000), so night and a day interior are only 2 stops apart.
    - The rods are full below -9.5 EV and the cones above -7, between the two; the street at night (-4.1, its lamps) is left in colour.
  - **One smooth step:** the light lab's smoothstep and the spot cone's were copies of GLSL's smoothstep, and night vision needed a third, so all three became one `smoothStep` in `math/smooth_step.h`.
  - **Golden images:** the phase contrast changed every light lab view with the moon up (by day too: the daytime crescent's disc and its faint light in the probes), and night vision the moonlit view and the demo's night views (only their final images; the daytime views and the street at night matched). The references were saved anew.
    - Run after the GPU tests (`make gputest`), two scene images differ by a few dozen pixels (street at night, the demo's lamp field), though tone mapping cannot reach scene images; `make golden` alone matches.
    - The street's is already on the roadmap as a golden view that lands on one of two pictures.
  - **Left for later:** the user then found the sky still too bright and the ground too dark under a dim moon. Two likely causes, neither fixed:
    - a crescent moon is up only near the sun, so its glow is partly the sun's twilight
    - under any dim moon the scene sits in the tone curve's toe with the exposure at its highest, the mean set by the sky, so the ground is crushed while the sky shows (a lower contrast at night, which rods have, was proposed).

    The user chose to leave the night's look for later, with physical light units (ROADMAP phase 14), and night vision is off by default; the golden references were saved with it on, then saved again with it off (`golden-night-vision-off`: the moonlit view and the demo's night views; the flaky street-night and floodlights scene captures of that run were left at their old references).

- `docs-readability` (Oct 23 2026): the user found the docs' very long, unspaced paragraphs hard to read (single lines up to 13,029 characters in `docs/ARCHITECTURE.md`).
  - **The style:** `README.md` was rewritten first as the sample and agreed: short paragraphs with one idea each, headings, bullets with short bold lead-ins, tables where data is tabular, formulas on their own lines.
  - **The rest:** `ROADMAP.md`, `docs/BUILDLOG.md`, `docs/ENGINE_GUIDE.md`, `docs/ARCHITECTURE.md`, `docs/research/ILLUMINATION.md`, `docs/research/LUMEN.md` and `docs/research/RAGDOLLPLAN.md`, one agent each in parallel and one commit each. Not `CLAUDE.md`, which is for Claude only.
  - **Nothing lost:** a script compared each rewrite with the committed version and listed every number, code span, link and heading that occurs fewer times. Every drop is a stale fact corrected or a duplicate merged (`ddgi-probe-relocation` was in this log twice, word for word). Every heading text is kept, since code and docs refer to sections by name.
  - **Longest line now:** 557 characters (the guide), from 13,029.
  - **Stale facts corrected on the way**, each checked in the code: 128 probe rays by default (not 64), every light seated by default, the fast lights' rays, the adaptive memory's 8 spreads, the presets' cascades, the probe cascades' defaults, the GPU pass names and readout line, `GpuFrameData`'s and `ToneMapUniformData`'s sizes, quaternions and the release build done (the ragdoll plan), the software tracer a distance field (not a voxel grid). Bigger stale parts of `docs/ARCHITECTURE.md` are a roadmap item.
  - **`tools/package_demo.py`:** the zip's controls come from the README's Controls section, whose long rows became short rows plus notes. In the brief (anonymous) controls a heading over notes alone is now left out, and wrapped list items indent under their text.

- `architecture-stale` (Oct 24 2026): the stale parts of `docs/ARCHITECTURE.md` the readability pass had left for later, each checked in the code.
  - **Lights and shading:** the section still described the renderer before the GPU scene. Lights are now three storage buffers (1024 point, 256 spot), not a 2720-byte uniform block of 64 point and 8 spot lights. Textures come from the bindless array and the scene set, not six fragment sampler slots. Handedness is in each object's instance, not pushed per draw, and the shading lives in `scene_surface.glsl`.
  - **Golden views:** the light lab has nine, with `street-night`. The two "all twelve golden images" are kept as what was checked then.
  - **Smaller fixes:** the demo's hover readouts are on lines 11 to 13, after its system timings on `firstGameReadoutLine` (10). The sentence on the order of `Application`'s members moved to its section. The tone mapping pass is `drawToneMap`, and the raw shadow atlas is scene set 1 binding 8.
  - **Also:** the engine guide's "Covers" list, a run-on sentence cut into bullets ("so all of 6c"), became one line per phase (`guide-covers-list`).

- `taa` (Oct 23 2026): temporal anti-aliasing, 6e's first item. Each pixel showed the scene at one point, so edges stepped and thin things (the light lab's fence) shimmered as the camera moved.
  - **How:** the main pass is jittered on a 16-frame Halton (2, 3) cycle, and each pixel keeps an exponential average of what it saw (each new frame weighted 0.1), reprojected to where its surface was last frame and clipped into the box of its 3x3 neighbours' colours (mean plus or minus one standard deviation, in YCoCg), each sample weighed by bloom's Karis weight, the history read with Catmull-Rom.
  - **Motion, decided:** a separate velocity pass that redraws only the objects that moved, as UE4 does, rather than a second colour output from the main pass. The sky and stars are drawn inside the main pass's rendering block, and every pipeline there must have the same colour attachments, so a velocity output would have changed all of them. Still things and the sky are reprojected from depth and the two cameras in the resolve instead.
  - **Last frame's matrices:** `MotionHistory` (owned by `Application`, render-only) remembers each model part's matrix by entity and part, so each `DrawItem` carries `previousModel`; the renderer keeps last frame's camera.
  - **The result goes back over the scene image** with a copy, so eye adaptation, bloom, tone mapping and captures read the smoothed picture without changes of their own.
  - **Reused:** bloom's `karisWeight` for the blend, and a general `inverse` added to `math/mat4` (Gauss-Jordan in doubles) for the reprojection, which is exactly the identity for a still camera.
  - **Captures** draw 32 frames first (two jitter cycles), restarting the history and the cycle, so golden images repeat exactly. Golden images were not run or saved in this branch (only on request): every view's final image will change.
  - **Checked:** the tests (debug and release), and the validation layers in the light lab and the demo (no messages). The GPU test cases for `taa.glsl` are written and built, not run (only on request).
